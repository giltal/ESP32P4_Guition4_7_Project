/*
 * WiFi station support.
 *
 * The ESP32-P4 has no radio. Every esp_wifi_* call here is serviced by the
 * ESP32-C6 inside the JC-ESP32P4-M3 module, reached over SDIO by ESP-Hosted;
 * esp_wifi_remote forwards the API across that link. Nothing in this header
 * is P4-specific as a result — it is the ordinary IDF station flow.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_WIFI_SSID_MAX 32
#define BSP_WIFI_PASS_MAX 64

typedef enum {
    BSP_WIFI_STOPPED,
    BSP_WIFI_DISCONNECTED,
    BSP_WIFI_CONNECTING,
    BSP_WIFI_CONNECTED,
} bsp_wifi_state_t;

/**
 * @brief Start the WiFi stack in station mode.
 *
 * Brings up netif, the default event loop and the C6 link, then leaves the
 * station idle. Does not connect — call bsp_wifi_connect() or
 * bsp_wifi_connect_saved(). Safe to call more than once.
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief Connect to an access point.
 *
 * Returns as soon as the attempt is under way; use bsp_wifi_wait_connected()
 * to block. While connected, drops are retried automatically with backoff.
 *
 * @param ssid      network name
 * @param password  passphrase, or NULL/"" for an open network
 * @param save      also store these credentials for bsp_wifi_connect_saved()
 */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password, bool save);

/** @brief Connect using credentials stored by bsp_wifi_connect(..., save=true). */
esp_err_t bsp_wifi_connect_saved(void);

/** @brief Whether credentials are stored, i.e. whether provisioning can be skipped. */
bool bsp_wifi_has_credentials(void);

/**
 * @brief Store credentials without touching the current connection.
 *
 * Use this to persist credentials that have just been proven to work, rather
 * than calling bsp_wifi_connect(..., save=true) a second time — that would
 * re-issue esp_wifi_connect() and drop the link it just established.
 */
esp_err_t bsp_wifi_save_credentials(const char *ssid, const char *password);

/** @brief Erase stored credentials. Does not disconnect. */
esp_err_t bsp_wifi_forget(void);

/** @brief Disconnect and stop automatic retries. */
esp_err_t bsp_wifi_disconnect(void);

/** @brief Block until connected, or until timeout_ms elapses. */
esp_err_t bsp_wifi_wait_connected(uint32_t timeout_ms);

/** @brief Current connection state. */
bsp_wifi_state_t bsp_wifi_state(void);

/** @brief Whether the station currently holds an IP address. */
bool bsp_wifi_is_connected(void);

/** @brief Copy the current IPv4 address. Fails unless connected. */
esp_err_t bsp_wifi_get_ip(esp_ip4_addr_t *out_ip);

/**
 * @brief Signal strength of the current AP in dBm.
 *
 * Returns the most recent sample taken by the internal link monitor rather
 * than querying the radio, so it never blocks and is safe to call from an
 * LVGL timer. Fails when there is no recent sample.
 */
esp_err_t bsp_wifi_get_rssi(int *out_rssi);

/**
 * @brief How many times the link monitor has rescued a silently dead link.
 *
 * The C6 does not always emit WIFI_EVENT_STA_DISCONNECTED when an association
 * dies, which would otherwise leave the device apparently connected but
 * unreachable. A rising count means that is happening.
 */
uint32_t bsp_wifi_link_recoveries(void);

/**
 * @brief Blocking scan for nearby access points.
 *
 * @param out_records  caller-provided array, filled with up to *io_count entries
 * @param io_count     in: array capacity; out: records actually written
 */
esp_err_t bsp_wifi_scan(wifi_ap_record_t *out_records, uint16_t *io_count);

#ifdef __cplusplus
}
#endif
