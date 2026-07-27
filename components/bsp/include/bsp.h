/*
 * Board Support Package — Guition JC4880P443C-I-W (ESP32-P4)
 *
 * Brings up the board at its native 480x800 orientation. There is no
 * virtual framebuffer and no legacy display shim: draw through the
 * st7701_lcd API directly, or hand the panel to LVGL.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Which subsystems bsp_init() should bring up. */
typedef struct {
    bool display;       /**< ST7701S MIPI-DSI panel */
    bool touch;         /**< GT911 (requires display) */
    bool audio;         /**< ES8311 codec over I2S */
    bool sdcard;        /**< SDMMC slot 0, mounted at BSP_SD_MOUNT_POINT */
    uint8_t brightness; /**< initial backlight, 0-100 (display only) */
} bsp_config_t;

#define BSP_CONFIG_DEFAULT() (bsp_config_t){ \
    .display    = true,                      \
    .touch      = true,                      \
    .audio      = true,                      \
    .sdcard     = true,                      \
    .brightness = 80,                        \
}

/**
 * @brief Bring up the board.
 *
 * Order: I2C bus -> PPA -> backlight -> panel -> touch -> audio -> SD.
 * Audio and SD failures are logged and skipped; display failure is fatal.
 * Safe to call more than once — subsequent calls are no-ops.
 */
esp_err_t bsp_init(const bsp_config_t *cfg);

/**
 * @brief Shared I2C master bus (port BSP_I2C_PORT), or NULL before bsp_init().
 *
 * Both the GT911 and the ES8311 hang off this bus. Add further devices with
 * i2c_master_bus_add_device() rather than creating a second bus.
 */
i2c_master_bus_handle_t bsp_i2c_bus(void);

/* ── Backlight ──────────────────────────────────────────────────── */

/** @brief Set backlight brightness, 0-100 percent. */
esp_err_t bsp_backlight_set(uint8_t percent);

/** @brief Last brightness passed to bsp_backlight_set(). */
uint8_t bsp_backlight_get(void);

/* ── SD card ────────────────────────────────────────────────────── */

/** @brief Mount SDMMC slot 0 at BSP_SD_MOUNT_POINT (retries with LDO power-cycle). */
esp_err_t bsp_sdcard_mount(void);

/** @brief Unmount the SD card. */
esp_err_t bsp_sdcard_unmount(void);

/** @brief Whether the SD card is currently mounted. */
bool bsp_sdcard_is_mounted(void);

#ifdef __cplusplus
}
#endif
