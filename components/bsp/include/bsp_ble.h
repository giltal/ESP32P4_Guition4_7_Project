/*
 * BLE peripheral support.
 *
 * NimBLE runs as the host on the ESP32-P4; the P4 has no BT controller, so
 * HCI is carried over VHCI on the same ESP-Hosted SDIO link to the C6 that
 * serves WiFi. Both radios share that transport — no extra wiring.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called on connect and disconnect. `conn_handle` is NimBLE's handle. */
typedef void (*bsp_ble_conn_cb_t)(uint16_t conn_handle, bool connected);

typedef struct {
    const char       *device_name;     /**< advertised name; NULL for "gution-p4" */
    uint32_t          adv_interval_ms; /**< advertising interval; 0 for 100 ms */
    bsp_ble_conn_cb_t on_conn_change;  /**< optional connection callback */

    /**
     * @brief Called to register application GATT services.
     *
     * Invoked after the standard GAP/GATT services are set up but before the
     * host task starts — the only window in which services may be added.
     */
    void (*register_services)(void);

    /**
     * @brief Ask the central to pair as soon as it connects.
     *
     * Windows never initiates pairing with a plain GATT peripheral — it
     * connects, enumerates and leaves — so no bond is ever created, and
     * Windows applications that only work with bonded devices cannot use it.
     * Requesting security from this side makes Windows pair instead.
     *
     * iOS and command-line clients pair transparently under Just Works, so
     * this is normally harmless; a peer that refuses is logged and keeps the
     * unencrypted connection, since nothing here demands encryption.
     */
    bool require_pairing;

    /**
     * @brief Optional 128-bit service UUID to advertise, 16 bytes.
     *
     * Most significant byte first, i.e. the order the UUID is written down.
     * Goes in the scan response rather than the advertisement: a 128-bit UUID
     * needs 18 bytes and the name another 11, which together overflow the
     * 31-byte legacy advertising packet.
     */
    const uint8_t    *adv_uuid128;
} bsp_ble_config_t;

#define BSP_BLE_CONFIG_DEFAULT() (bsp_ble_config_t){ \
    .device_name       = NULL,                       \
    .adv_interval_ms   = 0,                          \
    .on_conn_change    = NULL,                       \
    .register_services = NULL,                       \
    .require_pairing   = false,                      \
    .adv_uuid128       = NULL,                       \
}

/**
 * @brief Start the NimBLE host and begin advertising as a peripheral.
 *
 * Register any GATT services with the NimBLE API *before* calling this —
 * the host task starts here and advertising begins as soon as the stack
 * syncs with the controller on the C6.
 *
 * Requires the ESP-Hosted link, so call bsp_wifi_init() (or otherwise bring
 * the link up) first if you need a deterministic ordering; NimBLE will
 * otherwise sync once the transport is available.
 */
esp_err_t bsp_ble_init(const bsp_ble_config_t *cfg);

/** @brief Stop advertising and shut the NimBLE host down. */
esp_err_t bsp_ble_deinit(void);

/** @brief Begin advertising. Called for you by bsp_ble_init(). */
esp_err_t bsp_ble_advertise_start(void);

/** @brief Stop advertising without tearing the host down. */
esp_err_t bsp_ble_advertise_stop(void);

/** @brief Whether a central is currently connected. */
bool bsp_ble_is_connected(void);

/**
 * @brief Whether the controller has synced and is usable.
 *
 * False means the C6's controller never answered HCI, so nothing is being
 * advertised however healthy the rest of the system looks.
 */
bool bsp_ble_is_ready(void);

/** @brief Whether advertising is actually running right now. */
bool bsp_ble_is_advertising(void);

/** @brief Handle of the first active connection; meaningless unless connected. */
uint16_t bsp_ble_conn_handle(void);

/**
 * @brief Number of centrals currently connected.
 *
 * More than one is normal — a phone and a PC, say. Anything sending
 * notifications should walk all of them, not just the first.
 */
int bsp_ble_conn_count(void);

/** @brief Handle of the connection at `index`, or none if out of range. */
uint16_t bsp_ble_conn_handle_at(int index);

/** @brief This device's BLE address as a printable string, e.g. "aa:bb:…". */
esp_err_t bsp_ble_get_address(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
