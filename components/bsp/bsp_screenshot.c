/*
 * Screenshots to PNG — see bsp_screenshot.h.
 */

#include "bsp_screenshot.h"
#include "bsp.h"
#include "bsp_lvgl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"

#include "lvgl.h"

static const char *TAG = "bsp_shot";

#define LOCK_TIMEOUT_MS 2000

/* ── Checksums ──────────────────────────────────────────────────── */
/*
 * Bitwise rather than table-driven: about 9 M operations for a full screen,
 * a few tens of milliseconds on a 360 MHz core, against an SD write that
 * costs far more. Not worth a 1 KB table.
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

#define ADLER_MOD 65521u

static void adler32_update(uint32_t *a, uint32_t *b, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        *a = (*a + data[i]) % ADLER_MOD;
        *b = (*b + *a) % ADLER_MOD;
    }
}

/* ── Emit helpers ───────────────────────────────────────────────── */
typedef struct {
    bsp_screenshot_sink_t sink;
    void                 *ctx;
    uint32_t              crc;   /* running CRC of the current chunk */
    int                   err;
} writer_t;

static void emit(writer_t *w, const void *data, size_t len)
{
    if (w->err) return;
    w->crc = crc32_update(w->crc, data, len);
    if (w->sink(w->ctx, data, len) != 0) w->err = 1;
}

static void emit_be32(writer_t *w, uint32_t v)
{
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
    emit(w, b, 4);
}

/** Length and type are written outside the CRC, then CRC covers type+data. */
static void chunk_begin(writer_t *w, const char *type, uint32_t data_len)
{
    if (w->err) return;

    uint8_t len_be[4] = { data_len >> 24, data_len >> 16, data_len >> 8, data_len };
    if (w->sink(w->ctx, len_be, 4) != 0) { w->err = 1; return; }

    w->crc = 0;
    emit(w, type, 4);
}

static void chunk_end(writer_t *w)
{
    if (w->err) return;
    uint32_t crc = w->crc;
    uint8_t  b[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
    if (w->sink(w->ctx, b, 4) != 0) w->err = 1;
}

/* ── PNG encode ─────────────────────────────────────────────────── */
static esp_err_t encode_png(const lv_draw_buf_t *snap,
                            bsp_screenshot_sink_t sink, void *ctx)
{
    const uint32_t w      = snap->header.w;
    const uint32_t h      = snap->header.h;
    const uint32_t stride = snap->header.stride;
    const uint32_t rowlen = 1 + w * 3;   /* filter byte + RGB888 */

    uint8_t *row = malloc(rowlen);
    ESP_RETURN_ON_FALSE(row, ESP_ERR_NO_MEM, TAG, "row buffer");

    writer_t wr = { .sink = sink, .ctx = ctx, .crc = 0, .err = 0 };

    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (sink(ctx, SIG, sizeof(SIG)) != 0) { free(row); return ESP_FAIL; }

    /* IHDR: 8-bit truecolour, no interlace. */
    chunk_begin(&wr, "IHDR", 13);
    emit_be32(&wr, w);
    emit_be32(&wr, h);
    const uint8_t ihdr_tail[5] = { 8, 2, 0, 0, 0 };
    emit(&wr, ihdr_tail, sizeof(ihdr_tail));
    chunk_end(&wr);

    /*
     * One stored deflate block per row keeps the encoder trivial: each row is
     * 1441 bytes, far below the 65535-byte block limit, so no row ever has to
     * be split across blocks. Overhead is 5 bytes per row — 4 KB for a whole
     * screen, which is nothing against the 1.1 MB payload.
     */
    const uint32_t idat_len = 2 + h * (5 + rowlen) + 4;
    chunk_begin(&wr, "IDAT", idat_len);

    const uint8_t zlib_hdr[2] = { 0x78, 0x01 };   /* deflate, 32K window */
    emit(&wr, zlib_hdr, 2);

    uint32_t a = 1, b = 0;

    for (uint32_t y = 0; y < h && !wr.err; y++) {
        const uint16_t *src = (const uint16_t *)(snap->data + (size_t)y * stride);

        row[0] = 0;   /* filter: none */
        uint8_t *p = row + 1;
        for (uint32_t x = 0; x < w; x++) {
            uint16_t px = src[x];
            /* Replicate the top bits so 0x1F maps to 0xFF, not 0xF8. */
            uint8_t r = (px >> 11) & 0x1F;
            uint8_t g = (px >> 5)  & 0x3F;
            uint8_t bl =  px       & 0x1F;
            *p++ = (uint8_t)((r << 3) | (r >> 2));
            *p++ = (uint8_t)((g << 2) | (g >> 4));
            *p++ = (uint8_t)((bl << 3) | (bl >> 2));
        }

        uint8_t bh[5];
        bh[0] = (y == h - 1) ? 1 : 0;            /* BFINAL on the last row */
        bh[1] = rowlen & 0xFF;
        bh[2] = (rowlen >> 8) & 0xFF;
        bh[3] = ~bh[1];
        bh[4] = ~bh[2];
        emit(&wr, bh, 5);
        emit(&wr, row, rowlen);

        adler32_update(&a, &b, row, rowlen);
    }

    emit_be32(&wr, (b << 16) | a);
    chunk_end(&wr);

    chunk_begin(&wr, "IEND", 0);
    chunk_end(&wr);

    free(row);
    return wr.err ? ESP_FAIL : ESP_OK;
}

/* ── Capture ────────────────────────────────────────────────────── */
esp_err_t bsp_screenshot_write(bsp_screenshot_sink_t sink, void *ctx)
{
    ESP_RETURN_ON_FALSE(sink, ESP_ERR_INVALID_ARG, TAG, "no sink");

    /*
     * Capture under the lock, encode outside it. lv_snapshot_take renders into
     * a buffer it allocates for us, so once we have it the UI is free to carry
     * on while the (slow) SD write proceeds.
     */
    if (!bsp_lvgl_lock(LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "could not take the LVGL lock");
        return ESP_ERR_TIMEOUT;
    }
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_lvgl_unlock();

    ESP_RETURN_ON_FALSE(snap, ESP_ERR_NO_MEM, TAG, "snapshot failed");

    ESP_LOGI(TAG, "Captured %" PRIu32 "x%" PRIu32,
             (uint32_t)snap->header.w, (uint32_t)snap->header.h);

    esp_err_t err = encode_png(snap, sink, ctx);
    lv_draw_buf_destroy(snap);
    return err;
}

/* ── File output ────────────────────────────────────────────────── */
static int file_sink(void *ctx, const void *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

esp_err_t bsp_screenshot_save(const char *path)
{
    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "no path");
    ESP_RETURN_ON_FALSE(bsp_sdcard_is_mounted(), ESP_ERR_INVALID_STATE,
                        TAG, "no SD card");

    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot open %s", path);

    esp_err_t err = bsp_screenshot_write(file_sink, f);
    fclose(f);

    if (err != ESP_OK) {
        remove(path);   /* do not leave a truncated PNG behind */
        ESP_LOGE(TAG, "Failed writing %s", path);
        return err;
    }

    ESP_LOGI(TAG, "Saved %s", path);
    return ESP_OK;
}

/** Highest NNNN already present in shot_NNNN.png, or 0 when there are none. */
static int highest_index(void)
{
    DIR *d = opendir(BSP_SCREENSHOT_DIR);
    if (d == NULL) return 0;

    int            best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, "shot_%d.png", &n) == 1 && n > best) best = n;
    }

    closedir(d);
    return best;
}

esp_err_t bsp_screenshot_save_next(char *out_path, size_t out_len)
{
    ESP_RETURN_ON_FALSE(bsp_sdcard_is_mounted(), ESP_ERR_INVALID_STATE,
                        TAG, "no SD card");

    struct stat st;
    if (stat(BSP_SCREENSHOT_DIR, &st) != 0) {
        if (mkdir(BSP_SCREENSHOT_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "cannot create %s", BSP_SCREENSHOT_DIR);
            return ESP_FAIL;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/shot_%04d.png",
             BSP_SCREENSHOT_DIR, highest_index() + 1);

    esp_err_t err = bsp_screenshot_save(path);
    if (err == ESP_OK && out_path && out_len) {
        strlcpy(out_path, path, out_len);
    }
    return err;
}
