/*
 * LVGL binding for the 480x800 ST7701S panel and GT911 touch.
 *
 * esp_lvgl_port owns the LVGL task and tick; this just wires our panel and
 * touch driver into it. LVGL is not thread-safe, so anything touching the
 * widget tree from another task must hold the lock — see bsp_lvgl_lock().
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start LVGL and attach the display and touch input.
 *
 * Call after bsp_init() — the panel and touch controller must already exist.
 */
esp_err_t bsp_lvgl_init(void);

/** @brief The LVGL display, or NULL before bsp_lvgl_init(). */
lv_display_t *bsp_lvgl_display(void);

/**
 * @brief Take the LVGL lock before touching any widget from another task.
 *
 * Not needed inside LVGL event callbacks or lv_timer callbacks — those already
 * run under the lock. The mutex is recursive, so taking it again there is
 * harmless if it keeps a helper usable from both contexts.
 *
 * The corollary matters more: because timers and event callbacks hold this
 * lock, they must never block. A blocking call there — anything that talks to
 * the C6 over SDIO, or slow file I/O — stalls every other task waiting on the
 * lock, and since the idle tasks keep running no watchdog fires. Do that work
 * on a separate task and cache the result.
 *
 * @param timeout_ms  how long to wait; 0 waits indefinitely
 * @return true if the lock was taken; do not draw when false
 */
bool bsp_lvgl_lock(uint32_t timeout_ms);

/** @brief Release the LVGL lock. */
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
