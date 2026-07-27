/*
 * Per-network password store on the SD card.
 *
 * NVS (see bsp_wifi.h) remembers only the one network the device last joined,
 * which is what it auto-connects to at boot. This store keeps a password per
 * SSID, so picking a previously used network in the UI fills its password in
 * again instead of making someone retype it.
 *
 * SECURITY: passwords are written in plain text to a removable card. Anyone
 * holding the card can read every network's password. Fine for a lab or
 * classroom; not something to ship to customer sites.
 *
 * Format is one record per line, tab-separated, so it can be read and edited
 * on any machine:
 *
 *     # gution wifi credentials
 *     MyNetwork<TAB>my-wifi-password
 *
 * SSIDs or passwords containing a tab or newline cannot be represented and
 * are refused on save.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the file lives, on the SD mount point. */
#define BSP_WIFI_STORE_PATH "/sd/wifi_creds.txt"

/** Most networks remembered; older entries are kept, extras refused. */
#define BSP_WIFI_STORE_MAX_ENTRIES 16

/**
 * @brief Look up the stored password for an SSID.
 *
 * @return ESP_OK when found, ESP_ERR_NOT_FOUND when the SSID has no entry or
 *         the file does not exist, ESP_ERR_INVALID_STATE with no card mounted.
 */
esp_err_t bsp_wifi_store_lookup(const char *ssid, char *out, size_t out_len);

/**
 * @brief Save (or replace) the password for an SSID.
 *
 * Rewrites the file with the entry added or updated. Call it only once the
 * credentials are known to work.
 */
esp_err_t bsp_wifi_store_save(const char *ssid, const char *password);

/** @brief Remove one SSID's entry. ESP_OK even when there was nothing to remove. */
esp_err_t bsp_wifi_store_forget(const char *ssid);

/** @brief Whether an entry exists for this SSID. */
bool bsp_wifi_store_has(const char *ssid);

#ifdef __cplusplus
}
#endif
