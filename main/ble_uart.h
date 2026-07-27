/*
 * Serial port over BLE — the Nordic UART Service (NUS).
 *
 * NUS is not a Bluetooth SIG standard, but it is the de-facto one: nRF
 * Connect, LightBlue, Serial Bluetooth Terminal and most BLE libraries
 * recognise these UUIDs and offer a terminal, so a device carrying it needs no
 * custom client.
 *
 *   Service  6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX       6e400002-...   central -> device   (write)
 *   TX       6e400003-...   device -> central   (notify)
 *
 * The names are from the central's point of view, which is the usual source of
 * confusion: the device *receives* on RX and *sends* on TX.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 16 bytes, most significant first — for bsp_ble_config_t.adv_uuid128. */
extern const uint8_t ble_uart_service_uuid[16];

/**
 * @brief Called when a central writes to RX.
 *
 * Runs on the NimBLE host task, so keep it short: blocking here stalls the
 * whole BLE stack. `data` is not NUL-terminated and is only valid for the
 * duration of the call.
 */
typedef void (*ble_uart_rx_cb_t)(const uint8_t *data, size_t len, void *user);

/**
 * @brief Register the service with NimBLE.
 *
 * Must run in the window before the host task starts — see
 * bsp_ble_config_t.register_services.
 */
void ble_uart_register(void);

/** @brief Install the receive handler. Safe to call before or after register. */
void ble_uart_set_rx_handler(ble_uart_rx_cb_t cb, void *user);

/**
 * @brief Send bytes to the connected central.
 *
 * Split across notifications to fit the negotiated MTU. Fails when nothing is
 * connected; silently discarded by the stack if the central has not subscribed
 * to TX notifications.
 */
esp_err_t ble_uart_send(const void *data, size_t len);

/** @brief printf-style convenience wrapper over ble_uart_send(). */
esp_err_t ble_uart_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief Whether a central is connected and able to receive. */
bool ble_uart_is_connected(void);

#ifdef __cplusplus
}
#endif
