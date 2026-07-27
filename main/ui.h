/*
 * Touchscreen UI — status, WiFi setup and firmware update.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the UI and show it.
 *
 * Call after bsp_lvgl_init(). Takes the LVGL lock internally.
 */
esp_err_t ui_init(void);

/**
 * @brief Append a line to the on-screen BLE monitor.
 *
 * Logged raw, before any line assembly, so traffic is visible even when the
 * sender omits a newline — otherwise a client that never terminates a line
 * looks identical to one that is not connected at all.
 *
 * Safe to call from the NimBLE host task; takes the LVGL lock internally.
 *
 * @param text      the bytes, not necessarily NUL-terminated
 * @param len       length of `text`
 * @param incoming  true for data from the central, false for our replies
 */
void ui_ble_log(const char *text, size_t len, bool incoming);

/** Tabs, in the order they appear. */
typedef enum {
    UI_TAB_HOME = 0,
    UI_TAB_CONTROL,
    UI_TAB_BLE,
    UI_TAB_WIFI,
    UI_TAB_UPDATE,
    UI_TAB_COUNT,
} ui_tab_t;

/**
 * @brief Switch tabs from code.
 *
 * Lets a remote client drive the screen — useful for capturing documentation
 * screenshots, and for demos where nobody is standing at the board. Takes the
 * LVGL lock, so do not call it from an LVGL callback that already holds it.
 */
void ui_select_tab(ui_tab_t tab);

#ifdef __cplusplus
}
#endif
