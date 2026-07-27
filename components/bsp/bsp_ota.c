/*
 * Over-the-air firmware update — A/B between ota_0 and ota_1.
 */

#include "bsp_ota.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_ota";

#define DEFAULT_TIMEOUT_MS 10000
#define FILE_CHUNK_BYTES   4096

static bsp_ota_state_t s_state = BSP_OTA_IDLE;
static size_t          s_written;
static size_t          s_total;

/* ── Progress bookkeeping ───────────────────────────────────────── */
static void progress_reset(size_t total)
{
    s_written = 0;
    s_total   = total;
}

static void progress_report(const bsp_ota_config_t *cfg, size_t written)
{
    s_written = written;
    if (cfg->on_progress) cfg->on_progress(written, s_total, cfg->user);
}

bsp_ota_state_t bsp_ota_state(void) { return s_state; }

int bsp_ota_progress_percent(void)
{
    if (s_total == 0) return -1;
    return (int)((uint64_t)s_written * 100 / s_total);
}

/* ── Preconditions ──────────────────────────────────────────────── */
/*
 * esp_ota_set_boot_partition() refuses to run while the current image is still
 * on trial, so catch that here with a clear message rather than failing late
 * with ESP_ERR_OTA_ROLLBACK_INVALID_STATE after a full download.
 */
static esp_err_t check_can_update(void)
{
    if (bsp_ota_is_pending_verify()) {
        ESP_LOGE(TAG, "Running image is still pending verification — "
                      "call bsp_ota_mark_valid() before starting an update");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "No OTA partition available to write into");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Update target: '%s' at 0x%" PRIx32 " (%" PRIu32 " KB)",
             target->label, target->address, target->size / 1024);
    return ESP_OK;
}

/*
 * Reject an image carrying the version we are already running. Without this a
 * misconfigured update server can flip the device between slots indefinitely.
 */
static esp_err_t check_version(const esp_app_desc_t *incoming, const bsp_ota_config_t *cfg)
{
    if (cfg->allow_same_version) return ESP_OK;

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(incoming->version, running->version, sizeof(incoming->version)) == 0) {
        ESP_LOGW(TAG, "Image version \"%s\" matches the running image — refusing "
                      "(set allow_same_version to override)", incoming->version);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Incoming version \"%s\" (running \"%s\")",
             incoming->version, running->version);
    return ESP_OK;
}

static void finish(esp_err_t err, const bsp_ota_config_t *cfg)
{
    if (err != ESP_OK) {
        s_state = BSP_OTA_FAILED;
        ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
        return;
    }

    s_state = BSP_OTA_SUCCESS;
    ESP_LOGI(TAG, "Update written and boot partition switched");

    if (cfg->reboot_on_success) {
        ESP_LOGI(TAG, "Rebooting into the new image");
        vTaskDelay(pdMS_TO_TICKS(500));   /* let the log drain */
        esp_restart();
    }
}

/* ── Streaming core ─────────────────────────────────────────────── */
/*
 * The app descriptor sits at a fixed offset near the start of the image. A
 * chunked source may split that region across several writes, so bytes are
 * accumulated into a small header buffer until there are enough to validate.
 */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEADER_NEEDED   (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

static struct {
    bool                   active;
    esp_ota_handle_t       handle;
    const esp_partition_t *partition;
    bsp_ota_config_t       cfg;
    bool                   header_checked;
    uint8_t                header[HEADER_NEEDED];
    size_t                 header_len;
} s_stream;

bool bsp_ota_stream_active(void) { return s_stream.active; }

esp_err_t bsp_ota_stream_begin(size_t total_size, const bsp_ota_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_stream.active, ESP_ERR_INVALID_STATE, TAG,
                        "an update session is already open");

    bsp_ota_config_t def = BSP_OTA_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    ESP_RETURN_ON_ERROR(check_can_update(), TAG, "preconditions");

    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.cfg       = *cfg;
    s_stream.partition = esp_ota_get_next_update_partition(NULL);

    esp_err_t err = esp_ota_begin(s_stream.partition,
                                  total_size ? total_size : OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_stream.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    s_stream.active = true;
    s_state         = BSP_OTA_IN_PROGRESS;
    progress_reset(total_size);
    ESP_LOGI(TAG, "Update session open (%u bytes expected)", (unsigned)total_size);
    return ESP_OK;
}

esp_err_t bsp_ota_stream_write(const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_stream.active, ESP_ERR_INVALID_STATE, TAG, "no session");
    if (len == 0) return ESP_OK;

    /* Mirror the leading bytes into the header buffer until it is complete. */
    if (!s_stream.header_checked) {
        size_t want = HEADER_NEEDED - s_stream.header_len;
        size_t take = len < want ? len : want;
        memcpy(s_stream.header + s_stream.header_len, data, take);
        s_stream.header_len += take;

        if (s_stream.header_len == HEADER_NEEDED) {
            esp_app_desc_t incoming;
            memcpy(&incoming, s_stream.header + APP_DESC_OFFSET, sizeof(incoming));

            if (incoming.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                ESP_LOGE(TAG, "Payload is not a valid application image");
                return ESP_ERR_INVALID_ARG;
            }

            esp_err_t err = check_version(&incoming, &s_stream.cfg);
            if (err != ESP_OK) return err;

            s_stream.header_checked = true;
        }
    }

    esp_err_t err = esp_ota_write(s_stream.handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        return err;
    }

    progress_report(&s_stream.cfg, s_written + len);
    return ESP_OK;
}

esp_err_t bsp_ota_stream_end(void)
{
    ESP_RETURN_ON_FALSE(s_stream.active, ESP_ERR_INVALID_STATE, TAG, "no session");

    if (!s_stream.header_checked) {
        ESP_LOGE(TAG, "Image ended before a complete header arrived");
        bsp_ota_stream_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(s_stream.handle);
    s_stream.handle = 0;
    if (err != ESP_OK) {
        s_stream.active = false;
        finish(err, &s_stream.cfg);
        return err;
    }

    err = esp_ota_set_boot_partition(s_stream.partition);

    bsp_ota_config_t cfg = s_stream.cfg;
    s_stream.active = false;
    finish(err, &cfg);
    return err;
}

void bsp_ota_stream_abort(void)
{
    if (!s_stream.active) return;

    if (s_stream.handle) esp_ota_abort(s_stream.handle);
    s_stream.active = false;
    s_state         = BSP_OTA_FAILED;
    ESP_LOGW(TAG, "Update session aborted");
}

/* ── HTTP(S) source ─────────────────────────────────────────────── */
esp_err_t bsp_ota_update_from_url(const char *url, const bsp_ota_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(url && url[0], ESP_ERR_INVALID_ARG, TAG, "empty URL");

    bsp_ota_config_t def = BSP_OTA_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    ESP_RETURN_ON_ERROR(check_can_update(), TAG, "preconditions");

    ESP_LOGI(TAG, "Fetching %s", url);
    s_state = BSP_OTA_IN_PROGRESS;
    progress_reset(0);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = cfg->timeout_ms ? (int)cfg->timeout_ms : DEFAULT_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    if (cfg->server_cert_pem) {
        http_cfg.cert_pem = cfg->server_cert_pem;
    } else {
        /* Validate against IDF's bundled roots rather than skipping checks. */
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        finish(err != ESP_OK ? err : ESP_FAIL, cfg);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    esp_app_desc_t incoming;
    err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK) goto abort;

    err = check_version(&incoming, cfg);
    if (err != ESP_OK) goto abort;

    progress_reset((size_t)esp_https_ota_get_image_size(handle));

    /* esp_https_ota_perform() returns "in progress" until the body is drained. */
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        progress_report(cfg, (size_t)esp_https_ota_get_image_len_read(handle));
    }
    if (err != ESP_OK) goto abort;

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Connection closed before the whole image arrived");
        err = ESP_ERR_INVALID_SIZE;
        goto abort;
    }

    /* Verifies the image and sets the boot partition. */
    err = esp_https_ota_finish(handle);
    finish(err, cfg);
    return err;

abort:
    esp_https_ota_abort(handle);
    finish(err, cfg);
    return err;
}

/* ── SD-card source ─────────────────────────────────────────────── */
esp_err_t bsp_ota_update_from_file(const char *path, const bsp_ota_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(path && path[0], ESP_ERR_INVALID_ARG, TAG, "empty path");

    bsp_ota_config_t def = BSP_OTA_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    ESP_RETURN_ON_ERROR(check_can_update(), TAG, "preconditions");

    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", path);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= (long)sizeof(esp_image_header_t)) {
        ESP_LOGE(TAG, "%s is too small to be a firmware image (%ld bytes)", path, size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Updating from %s (%ld KB)", path, size / 1024);

    uint8_t *buf = malloc(FILE_CHUNK_BYTES);
    if (buf == NULL) {
        fclose(f);
        finish(ESP_ERR_NO_MEM, cfg);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bsp_ota_stream_begin((size_t)size, cfg);
    if (err != ESP_OK) {
        free(buf);
        fclose(f);
        return err;
    }

    size_t written = 0;
    for (;;) {
        size_t n = fread(buf, 1, FILE_CHUNK_BYTES, f);
        if (n == 0) break;

        err = bsp_ota_stream_write(buf, n);
        if (err != ESP_OK) goto abort_session;

        written += n;
    }

    if (written != (size_t)size) {
        ESP_LOGE(TAG, "Short read: %u of %ld bytes", (unsigned)written, size);
        err = ESP_ERR_INVALID_SIZE;
        goto abort_session;
    }

    free(buf);
    fclose(f);
    /* Verifies the image, switches the boot partition and may reboot. */
    return bsp_ota_stream_end();

abort_session:
    bsp_ota_stream_abort();
    free(buf);
    fclose(f);
    finish(err, cfg);
    return err;
}

/* ── Rollback ───────────────────────────────────────────────────── */
bool bsp_ota_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t bsp_ota_mark_valid(void)
{
    if (!bsp_ota_is_pending_verify()) return ESP_OK;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Running image confirmed — rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Could not confirm image: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_ota_rollback_and_reboot(void)
{
    ESP_LOGW(TAG, "Rolling back to the previous image");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only reached if there is no valid image to fall back to. */
    ESP_LOGE(TAG, "Rollback refused: %s", esp_err_to_name(err));
    return err;
}

/* ── Status ─────────────────────────────────────────────────────── */
esp_err_t bsp_ota_get_running_version(char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(out && out_len, ESP_ERR_INVALID_ARG, TAG, "bad args");
    strlcpy(out, esp_app_get_description()->version, out_len);
    return ESP_OK;
}

const char *bsp_ota_running_partition_label(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->label : "?";
}
