/*
 * Line-oriented console on top of the BLE UART service.
 *
 * Gives the terminal in nRF Connect (or any NUS client) something to talk to:
 * a handful of commands over the shared control state, so the same values
 * driven by the touchscreen and the web page can be read and written by typing.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Attach the console to the BLE UART. Call after ble_uart_register(). */
void ble_console_start(void);

/**
 * @brief Send a line from the device to the connected central.
 *
 * The reverse of the console: text typed on the touchscreen arriving in the
 * phone's or PC's terminal. A newline is appended.
 *
 * Queued rather than transmitted inline, and deliberately so. LVGL event
 * handlers run holding the LVGL lock, while NimBLE callbacks run holding the
 * host lock and reach into LVGL to update the monitor. Sending directly from a
 * button handler would take those two locks in the opposite order from the
 * receive path and can deadlock. A worker task takes them one at a time.
 *
 * Safe to call from any task, including LVGL callbacks. Silently dropped if
 * nothing is connected or the queue is full.
 */
void ble_console_send(const char *text);

#ifdef __cplusplus
}
#endif
