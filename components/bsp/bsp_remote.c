/*
 * Shared control state — see bsp_remote.h.
 */

#include "bsp_remote.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include <stdio.h>

#include "bsp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_remote";

#define MAX_SUBSCRIBERS 4

/*
 * Stored on the SD card rather than in NVS: it is plain text that can be read
 * or edited on any machine, which suits a teaching platform, and it keeps
 * settings with the card rather than the board. The cost is that a device with
 * no card simply does not persist — reported at startup rather than hidden.
 */
#define STATE_PATH "/sd/control_state.txt"
#define STATE_TMP  STATE_PATH ".tmp"

/*
 * Writes are coalesced rather than issued per change. Dragging the slider
 * produces a change per pixel, and committing each one would wear the flash
 * out for no benefit — only the value it settles on matters. A save happens
 * once the state has been quiet for this long.
 */
#define SAVE_QUIET_MS 3000

static SemaphoreHandle_t  s_lock;
static bsp_remote_state_t s_state;
static TaskHandle_t       s_saver;

static struct {
    bsp_remote_cb_t cb;
    void           *user;
} s_subs[MAX_SUBSCRIBERS];

static const char *const MODE_NAMES[BSP_REMOTE_MODE_COUNT] = {
    "Idle", "Run", "Test",
};

const char *bsp_remote_mode_name(bsp_remote_mode_t mode)
{
    if (mode < 0 || mode >= BSP_REMOTE_MODE_COUNT) return "?";
    return MODE_NAMES[mode];
}

/* ── Persistence ────────────────────────────────────────────────── */
/*
 * key=value lines rather than a struct dumped to disk: a binary image ties the
 * file to the C layout, so adding a field would silently invalidate every
 * saved state. With named keys, unknown ones are ignored and missing ones keep
 * their defaults — and the file stays readable.
 */
static void state_save(void)
{
    if (!bsp_sdcard_is_mounted()) return;

    bsp_remote_state_t s;
    bsp_remote_get(&s);

    /* Write then swap, so an interruption leaves the previous file intact
     * rather than a half-written one. */
    FILE *f = fopen(STATE_TMP, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "Cannot write %s", STATE_TMP);
        return;
    }

    fprintf(f, "# gution control state\n");
    fprintf(f, "mode=%d\n",  (int)s.mode);
    fprintf(f, "level=%d\n", s.level);
    fprintf(f, "led=%d\n",   s.led ? 1 : 0);
    fprintf(f, "label=%s\n", s.label);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* FATFS rename() will not overwrite, so drop the old file first. */
    remove(STATE_PATH);
    if (rename(STATE_TMP, STATE_PATH) != 0) {
        ESP_LOGW(TAG, "Could not put %s in place", STATE_PATH);
        return;
    }

    ESP_LOGI(TAG, "State saved to %s", STATE_PATH);
}

/** Load whatever is stored; anything missing keeps the default already set. */
static void state_load(void)
{
    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGW(TAG, "No SD card — state will not persist");
        return;
    }

    FILE *f = fopen(STATE_PATH, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No saved state — using defaults");
        return;
    }

    char line[BSP_REMOTE_LABEL_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        /* Strip the newline so it does not end up inside the label. */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';

        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "mode") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < BSP_REMOTE_MODE_COUNT) s_state.mode = (bsp_remote_mode_t)v;
        } else if (strcmp(key, "level") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 100) s_state.level = v;
        } else if (strcmp(key, "led") == 0) {
            s_state.led = (atoi(val) != 0);
        } else if (strcmp(key, "label") == 0) {
            strlcpy(s_state.label, val, sizeof(s_state.label));
        }
    }

    fclose(f);

    ESP_LOGI(TAG, "Restored from %s: mode=%s level=%d led=%s label=\"%s\"",
             STATE_PATH, bsp_remote_mode_name(s_state.mode), s_state.level,
             s_state.led ? "on" : "off", s_state.label);
}

/*
 * Saves on its own task rather than from a timer callback: an NVS commit can
 * block for a while when it triggers a flash erase, and the esp_timer task is
 * shared with the WiFi reconnect backoff among others.
 *
 * The inner take restarts the quiet period every time another change arrives,
 * so a burst collapses into a single write once things settle.
 */
static void saver_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_QUIET_MS)) > 0) {
            /* another change landed — keep waiting */
        }
        state_save();
    }
}

esp_err_t bsp_remote_init(void)
{
    if (s_lock) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_state.mode     = BSP_REMOTE_MODE_IDLE;
    s_state.level    = 50;
    s_state.led      = false;
    s_state.revision = 1;
    strlcpy(s_state.label, "Hello", sizeof(s_state.label));

    /* Before anything subscribes, so the first UI paint shows the real values.
     * revision deliberately restarts at 1 — it identifies changes within a
     * session, and pollers compare it against what they last saw. */
    state_load();

    if (xTaskCreate(saver_task, "remote_save", 3072, NULL, 2, &s_saver) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the saver task — state will not persist");
        s_saver = NULL;
    }

    ESP_LOGI(TAG, "Control state ready");
    return ESP_OK;
}

void bsp_remote_get(bsp_remote_state_t *out)
{
    if (out == NULL || s_lock == NULL) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void bsp_remote_set(const bsp_remote_state_t *in)
{
    if (in == NULL || s_lock == NULL) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_state.mode  = (in->mode >= 0 && in->mode < BSP_REMOTE_MODE_COUNT)
                        ? in->mode : BSP_REMOTE_MODE_IDLE;
    s_state.level = in->level < 0 ? 0 : (in->level > 100 ? 100 : in->level);
    s_state.led   = in->led;
    strlcpy(s_state.label, in->label, sizeof(s_state.label));
    s_state.revision++;

    /* Copy before releasing so subscribers see a consistent snapshot. */
    bsp_remote_state_t snapshot = s_state;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "mode=%s level=%d led=%s label=\"%s\" (rev %" PRIu32 ")",
             bsp_remote_mode_name(snapshot.mode), snapshot.level,
             snapshot.led ? "on" : "off", snapshot.label, snapshot.revision);

    /* Called outside the lock: a subscriber that reads the state back must
     * not deadlock against the write that woke it. */
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].cb) s_subs[i].cb(&snapshot, s_subs[i].user);
    }

    /* Nudge the saver; it writes once the changes stop. */
    if (s_saver) xTaskNotifyGive(s_saver);
}

esp_err_t bsp_remote_subscribe(bsp_remote_cb_t cb, void *user)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "null callback");

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].cb == NULL) {
            s_subs[i].cb   = cb;
            s_subs[i].user = user;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "no free subscriber slot");
    return ESP_ERR_NO_MEM;
}
