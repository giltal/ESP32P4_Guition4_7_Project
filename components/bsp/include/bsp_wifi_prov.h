/*
 * First-run WiFi provisioning over SoftAP.
 *
 * A device with no stored credentials cannot reach anything — not the update
 * server, not the upload page. This brings up an access point with a setup
 * page on it: join the AP from a phone or laptop, pick a network, enter the
 * password. Credentials are saved, the AP shuts down and the station connects.
 *
 * No companion app is required, unlike BLE provisioning.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ap_ssid;      /**< AP name; NULL for "gution-setup" */
    const char *ap_password;  /**< AP password, min 8 chars; NULL for an open AP */
    bool        captive_dns;  /**< answer all DNS queries so phones auto-open the page */
    uint32_t    timeout_ms;   /**< give up and stop after this long; 0 to wait forever */
} bsp_wifi_prov_config_t;

#define BSP_WIFI_PROV_CONFIG_DEFAULT() (bsp_wifi_prov_config_t){ \
    .ap_ssid     = NULL,                                         \
    .ap_password = NULL,                                         \
    .captive_dns = true,                                         \
    .timeout_ms  = 0,                                            \
}

/** @brief Whether provisioning is required, i.e. no credentials are stored. */
bool bsp_wifi_prov_is_needed(void);

/**
 * @brief Bring up the setup access point and its page. Returns immediately.
 *
 * The device runs in AP+STA mode so it can scan for networks while the AP is
 * up. Once a network is joined the AP and its page are torn down.
 *
 * The setup page listens on port 80, so it cannot run at the same time as the
 * firmware upload page — start that only after provisioning completes.
 */
esp_err_t bsp_wifi_prov_start(const bsp_wifi_prov_config_t *cfg);

/** @brief Tear down the access point and page early. */
esp_err_t bsp_wifi_prov_stop(void);

/** @brief Whether the setup access point is currently up. */
bool bsp_wifi_prov_is_active(void);

/**
 * @brief Block until the device is provisioned and connected.
 *
 * @return ESP_OK once connected, ESP_ERR_TIMEOUT if timeout_ms elapses first.
 */
esp_err_t bsp_wifi_prov_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
