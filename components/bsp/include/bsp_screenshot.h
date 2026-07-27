/*
 * Screenshots — capture what is actually on the panel, as a real PNG.
 *
 * LVGL renders the active screen into a buffer (lv_snapshot), which is then
 * encoded to PNG and either written to the SD card or streamed somewhere else,
 * such as straight to a browser.
 *
 * The PNG is written with *stored* (uncompressed) deflate blocks. That is a
 * fully standard PNG which any viewer opens, and it needs only CRC32 and
 * Adler32 rather than a compressor — the bundled `pngdec` is decode-only, and
 * pulling in zlib's deflate just to save a screenshot is not worth it. The
 * cost is file size: roughly 1.1 MB for a 480x800 screen.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Directory used by bsp_screenshot_save_next(). */
#define BSP_SCREENSHOT_DIR "/sd/shots"

/**
 * @brief Where encoded bytes go.
 *
 * @return 0 to continue, anything else to abort the encode.
 */
typedef int (*bsp_screenshot_sink_t)(void *ctx, const void *data, size_t len);

/**
 * @brief Capture the active screen and encode it to `sink`.
 *
 * Takes the LVGL lock only for the capture itself, then releases it before
 * encoding — writing a megabyte to an SD card takes long enough that holding
 * the lock throughout would visibly freeze the UI.
 *
 * Safe to call from any task except an LVGL event callback, which already
 * holds the lock (it is not recursive).
 */
esp_err_t bsp_screenshot_write(bsp_screenshot_sink_t sink, void *ctx);

/** @brief Capture the active screen to a PNG file, e.g. "/sd/shot.png". */
esp_err_t bsp_screenshot_save(const char *path);

/**
 * @brief Save to the next free BSP_SCREENSHOT_DIR/shot_NNNN.png.
 *
 * Creates the directory if needed. The path used is copied to `out_path`
 * when that is non-NULL.
 */
esp_err_t bsp_screenshot_save_next(char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif
