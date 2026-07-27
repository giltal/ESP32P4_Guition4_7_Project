/*
 * Shared control state between the web panel and the on-screen UI.
 *
 * One small struct that either side may read or write. A change made in a
 * browser shows up on the display, and a change made on the touchscreen shows
 * up in the browser — which is the point of the exercise: the same state seen
 * from two places.
 *
 * Every field is deliberately simple so a student can add another one by
 * copying an existing one through bsp_remote.c, the web page and ui.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_REMOTE_LABEL_MAX 32

/** Named positions for the radio group. */
typedef enum {
    BSP_REMOTE_MODE_IDLE = 0,
    BSP_REMOTE_MODE_RUN,
    BSP_REMOTE_MODE_TEST,
    BSP_REMOTE_MODE_COUNT,
} bsp_remote_mode_t;

typedef struct {
    bsp_remote_mode_t mode;                            /**< radio selection */
    int               level;                           /**< slider, 0-100 */
    bool              led;                             /**< on/off switch */
    char              label[BSP_REMOTE_LABEL_MAX + 1]; /**< free text */
    uint32_t          revision;                        /**< bumped on every change */
} bsp_remote_state_t;

/** Called after any change, on the task that made it. Never holds the lock. */
typedef void (*bsp_remote_cb_t)(const bsp_remote_state_t *state, void *user);

/** @brief Create the state and its lock. Safe to call more than once. */
esp_err_t bsp_remote_init(void);

/** @brief Copy out the current state. */
void bsp_remote_get(bsp_remote_state_t *out);

/**
 * @brief Replace the state and notify subscribers.
 *
 * `revision` in `in` is ignored — it is incremented internally, so a poller
 * can tell whether anything actually changed since it last looked.
 */
void bsp_remote_set(const bsp_remote_state_t *in);

/**
 * @brief Register a change subscriber.
 *
 * Both the display and any future consumer can subscribe; there is a small
 * fixed number of slots.
 */
esp_err_t bsp_remote_subscribe(bsp_remote_cb_t cb, void *user);

/** @brief Human-readable name for a mode, e.g. for labels. */
const char *bsp_remote_mode_name(bsp_remote_mode_t mode);

#ifdef __cplusplus
}
#endif
