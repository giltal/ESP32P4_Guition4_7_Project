/*
 * Firmware upload page served by the device itself.
 *
 * Point a browser at the board's IP address, choose a .bin, watch the progress
 * bar. No update server and no SD card involved — useful on a bench or at a
 * site where the device is reachable but nothing else is.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t    port;      /**< listening port; 0 for 80 */
    const char *username;  /**< HTTP Basic user; NULL disables authentication */
    const char *password;  /**< HTTP Basic password */
} bsp_ota_web_config_t;

#define BSP_OTA_WEB_CONFIG_DEFAULT() (bsp_ota_web_config_t){ \
    .port     = 0,                                           \
    .username = NULL,                                        \
    .password = NULL,                                        \
}

/**
 * @brief Start the upload server. Requires a network connection.
 *
 * Serves the upload page at `/`, device details at `/info`, and accepts the
 * image as the raw body of `POST /update`. On success the response is sent
 * first and the board reboots about a second later, so the browser can show
 * the result.
 *
 * With `username` set, every endpoint requires HTTP Basic credentials. Leaving
 * it NULL lets anyone who can reach the port install firmware — reasonable on
 * a bench, not on a shared network.
 */
esp_err_t bsp_ota_web_start(const bsp_ota_web_config_t *cfg);

/** @brief Stop the server. */
esp_err_t bsp_ota_web_stop(void);

/** @brief Whether the server is currently listening. */
bool bsp_ota_web_is_running(void);

#ifdef __cplusplus
}
#endif
