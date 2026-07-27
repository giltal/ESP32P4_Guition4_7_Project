/*
 * Over-the-air firmware update.
 *
 * One application image, written alternately into ota_0 and ota_1. The running
 * slot downloads into the other, flips the boot pointer and reboots; the new
 * image must then confirm itself or the bootloader reverts. See partitions.csv.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_OTA_IDLE,
    BSP_OTA_IN_PROGRESS,
    BSP_OTA_SUCCESS,
    BSP_OTA_FAILED,
} bsp_ota_state_t;

/**
 * @brief Download progress.
 *
 * @param written  bytes written so far
 * @param total    total image size, or 0 when the server sends no length
 * @param user     the `user` pointer from bsp_ota_config_t
 */
typedef void (*bsp_ota_progress_cb_t)(size_t written, size_t total, void *user);

typedef struct {
    bsp_ota_progress_cb_t on_progress;
    void    *user;
    bool     reboot_on_success;  /**< reboot into the new image when done */
    bool     allow_same_version; /**< by default an identical version is refused */
    uint32_t timeout_ms;         /**< network timeout; 0 for 10 s */
    const char *server_cert_pem; /**< HTTPS root cert; NULL uses the IDF bundle */
} bsp_ota_config_t;

#define BSP_OTA_CONFIG_DEFAULT() (bsp_ota_config_t){ \
    .on_progress        = NULL,                      \
    .user               = NULL,                      \
    .reboot_on_success  = true,                      \
    .allow_same_version = false,                     \
    .timeout_ms         = 0,                         \
    .server_cert_pem    = NULL,                      \
}

/**
 * @brief Update from an HTTP(S) URL. Blocking; takes seconds to minutes.
 *
 * Requires a working network connection (see bsp_wifi.h). Only returns on
 * failure when `reboot_on_success` is set.
 */
esp_err_t bsp_ota_update_from_url(const char *url, const bsp_ota_config_t *cfg);

/**
 * @brief Update from a firmware image on the SD card, e.g. "/sd/firmware.bin".
 *
 * For sites with no network path to the device.
 */
esp_err_t bsp_ota_update_from_file(const char *path, const bsp_ota_config_t *cfg);

/* ── Streaming ──────────────────────────────────────────────────── */
/*
 * For sources that arrive in pieces — an HTTP upload, a BLE transfer. Only one
 * session may be open at a time. The app descriptor is validated as soon as
 * enough bytes have accumulated, so chunk boundaries do not matter.
 */

/**
 * @brief Open an update session.
 *
 * @param total_size  full image size, or 0 if not known in advance
 */
esp_err_t bsp_ota_stream_begin(size_t total_size, const bsp_ota_config_t *cfg);

/**
 * @brief Append the next piece of the image.
 *
 * Fails once the image is recognised as invalid or the wrong version; abort
 * the session and stop sending when it does.
 */
esp_err_t bsp_ota_stream_write(const void *data, size_t len);

/**
 * @brief Finish: verify the image and switch the boot partition.
 *
 * Reboots when the session's config had `reboot_on_success` set.
 */
esp_err_t bsp_ota_stream_end(void);

/** @brief Discard the session and leave the boot partition untouched. */
void bsp_ota_stream_abort(void);

/** @brief Whether a streaming session is currently open. */
bool bsp_ota_stream_active(void);

/* ── Rollback ───────────────────────────────────────────────────── */

/**
 * @brief Whether this boot is the new image's single trial run.
 *
 * True only between an update and the first bsp_ota_mark_valid(). While true,
 * any reset reverts to the previous slot.
 */
bool bsp_ota_is_pending_verify(void);

/**
 * @brief Confirm the running image, cancelling the pending rollback.
 *
 * Call once the application has satisfied itself that this build works.
 * A no-op when the image is not pending verification.
 */
esp_err_t bsp_ota_mark_valid(void);

/**
 * @brief Abandon the running image and reboot into the previous one.
 *
 * Does not return on success.
 */
esp_err_t bsp_ota_rollback_and_reboot(void);

/* ── Status ─────────────────────────────────────────────────────── */

/** @brief State of the most recent update attempt. */
bsp_ota_state_t bsp_ota_state(void);

/** @brief Progress of the running update, 0-100, or -1 if the size is unknown. */
int bsp_ota_progress_percent(void);

/** @brief Version string of the running image (from the project version). */
esp_err_t bsp_ota_get_running_version(char *out, size_t out_len);

/** @brief Label of the partition the running image booted from. */
const char *bsp_ota_running_partition_label(void);

#ifdef __cplusplus
}
#endif
