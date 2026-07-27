/*
 * Touchscreen UI.
 *
 * Everything here runs on the LVGL task except the worker tasks at the bottom:
 * scanning, connecting and updating all block for seconds, and doing them in
 * an event callback would freeze the display. Workers touch widgets only
 * while holding the LVGL lock.
 */

#include "ui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include "bsp.h"
#include "bsp_lvgl.h"
#include "bsp_wifi.h"
#include "bsp_wifi_store.h"
#include "bsp_wifi_prov.h"
#include "bsp_ble.h"
#include "bsp_ota.h"
#include "bsp_ota_web.h"
#include "bsp_remote.h"
#include "bsp_screenshot.h"
#include "ble_console.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define SCAN_MAX_APS   20
#define LOCK_TIMEOUT   1000
#define PAD            12

/* Home tab */
static lv_obj_t *s_lbl_version;
static lv_obj_t *s_lbl_slot;
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_lbl_ble;
static lv_obj_t *s_lbl_sd;

/* WiFi tab */
static lv_obj_t *s_ap_list;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_btn_scan;
static lv_obj_t *s_btn_connect;
static lv_obj_t *s_lbl_wifi_status;
static char      s_sel_ssid[BSP_WIFI_SSID_MAX + 1];

/* Update tab */
static lv_obj_t *s_bar_ota;
static lv_obj_t *s_lbl_ota;
static lv_obj_t *s_btn_ota;
static lv_obj_t *s_lbl_upload_url;

static volatile bool s_worker_busy;

/* Share of the screen the on-screen keyboard occupies. The content area is
 * shrunk to the remainder while it is up. */
#define KEYBOARD_PCT 45

static lv_obj_t *s_tabs;

static void on_ap_clicked(lv_event_t *e);
static void show_keyboard_for(lv_obj_t *target);
static void hide_keyboard(void);

/* ── Helpers ────────────────────────────────────────────────────── */
static void set_text(lv_obj_t *label, const char *fmt, ...)
{
    if (label == NULL) return;
    va_list args;
    va_start(args, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lv_label_set_text(label, buf);
}

/** Update a label from a worker task, taking the lock. */
static void set_text_locked(lv_obj_t *label, const char *text)
{
    if (!bsp_lvgl_lock(LOCK_TIMEOUT)) return;
    if (label) lv_label_set_text(label, text);
    bsp_lvgl_unlock();
}

static lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    return card;
}

/** A "Label            value" row; returns the value label. */
static lv_obj_t *make_row(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *key = lv_label_create(row);
    lv_label_set_text(key, name);
    lv_obj_set_style_text_opa(key, LV_OPA_70, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "-");
    return val;
}

/* ── Home tab ───────────────────────────────────────────────────── */
/*
 * Everything drawn here must be non-blocking.
 *
 * lv_timer callbacks run inside lv_timer_handler() with the LVGL lock held, so
 * a call that stalls leaves that lock held forever. Every other task then
 * blocks on it while the idle tasks keep running, so no watchdog fires: the
 * device goes silent and stops answering without ever rebooting.
 *
 * All of these are safe — plain local state, or in the case of RSSI a value
 * cached by bsp_wifi's link monitor rather than an RPC to the C6.
 */
static void refresh_home(lv_timer_t *timer)
{
    (void)timer;

    set_text(s_lbl_version, "%s", esp_app_get_description()->version);
    set_text(s_lbl_slot, "%s", bsp_ota_running_partition_label());
    set_text(s_lbl_sd, "%s", bsp_sdcard_is_mounted() ? "mounted" : "none");
    /* Report what is actually happening — claiming "advertising" whenever no
     * central is connected hides a controller that never came up at all. */
    const char *ble_state = bsp_ble_is_connected()   ? "connected"
                          : bsp_ble_is_advertising() ? "advertising"
                          : bsp_ble_is_ready()       ? "idle"
                                                     : "unavailable";
    set_text(s_lbl_ble, "%s", ble_state);

    switch (bsp_wifi_state()) {
    case BSP_WIFI_CONNECTED: {
        int rssi = 0;
        if (bsp_wifi_get_rssi(&rssi) == ESP_OK) set_text(s_lbl_wifi, "%d dBm", rssi);
        else                                    set_text(s_lbl_wifi, "connected");

        esp_ip4_addr_t ip;
        if (bsp_wifi_get_ip(&ip) == ESP_OK) {
            set_text(s_lbl_ip, IPSTR, IP2STR(&ip));
            set_text(s_lbl_upload_url, "http://" IPSTR "/", IP2STR(&ip));
        }
        break;
    }
    case BSP_WIFI_CONNECTING:
        set_text(s_lbl_wifi, "connecting");
        set_text(s_lbl_ip, "-");
        break;
    default:
        set_text(s_lbl_wifi, "offline");
        set_text(s_lbl_ip, "-");
        break;
    }
}

/* ── Screenshot to SD ───────────────────────────────────────────── */
/*
 * Delayed on purpose. A screenshot taken the instant the button is pressed
 * can only ever capture the Home tab; the countdown gives time to switch to
 * whichever tab is wanted. The capture itself runs on a worker task because
 * encoding and writing ~1.1 MB to the card takes about a second.
 */
#define SHOT_DELAY_SECONDS 5

static lv_obj_t *s_btn_shot;
static lv_obj_t *s_lbl_shot;
static int       s_shot_countdown;

static void shot_worker(void *arg)
{
    (void)arg;

    char      path[64] = {0};
    esp_err_t err      = bsp_screenshot_save_next(path, sizeof(path));

    if (bsp_lvgl_lock(LOCK_TIMEOUT)) {
        if (err == ESP_OK) {
            set_text(s_lbl_shot, "Saved %s", path);
        } else if (err == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(s_lbl_shot, "No SD card");
        } else {
            set_text(s_lbl_shot, "Failed: %s", esp_err_to_name(err));
        }
        lv_obj_remove_state(s_btn_shot, LV_STATE_DISABLED);
        bsp_lvgl_unlock();
    }

    s_worker_busy = false;
    vTaskDelete(NULL);
}

static void shot_tick(lv_timer_t *timer)
{
    if (--s_shot_countdown > 0) {
        set_text(s_lbl_shot, "Capturing in %d...", s_shot_countdown);
        return;
    }

    lv_timer_delete(timer);
    lv_label_set_text(s_lbl_shot, "Capturing...");

    if (xTaskCreate(shot_worker, "ui_shot", 4096, NULL, 4, NULL) != pdPASS) {
        lv_label_set_text(s_lbl_shot, "Could not start capture");
        lv_obj_remove_state(s_btn_shot, LV_STATE_DISABLED);
        s_worker_busy = false;
    }
}

static void on_shot_clicked(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;
    s_worker_busy = true;

    lv_obj_add_state(s_btn_shot, LV_STATE_DISABLED);
    s_shot_countdown = SHOT_DELAY_SECONDS;
    set_text(s_lbl_shot, "Capturing in %d... switch tabs now", s_shot_countdown);
    lv_timer_create(shot_tick, 1000, NULL);
}

static void build_home(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, PAD, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "Guition P4");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *sub = lv_label_create(tab);
    lv_label_set_text(sub, "JC4880P443C-I-W");
    lv_obj_set_style_text_opa(sub, LV_OPA_60, 0);
    lv_obj_set_style_pad_bottom(sub, PAD, 0);

    lv_obj_t *card = make_card(tab);
    s_lbl_version = make_row(card, "Firmware");
    s_lbl_slot    = make_row(card, "Slot");
    s_lbl_wifi    = make_row(card, "WiFi");
    s_lbl_ip      = make_row(card, "IP");
    s_lbl_ble     = make_row(card, "Bluetooth");
    s_lbl_sd      = make_row(card, "SD card");

    s_btn_shot = lv_button_create(tab);
    lv_obj_set_width(s_btn_shot, LV_PCT(100));
    lv_obj_set_style_margin_top(s_btn_shot, PAD, 0);
    lv_obj_add_event_cb(s_btn_shot, on_shot_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *shot_lbl = lv_label_create(s_btn_shot);
    lv_label_set_text(shot_lbl, LV_SYMBOL_IMAGE "  Screenshot to SD");
    lv_obj_center(shot_lbl);

    s_lbl_shot = lv_label_create(tab);
    lv_label_set_text(s_lbl_shot, "Saves to /sd/shots after a 5 s countdown");
    lv_label_set_long_mode(s_lbl_shot, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_shot, LV_PCT(100));
    lv_obj_set_style_text_opa(s_lbl_shot, LV_OPA_60, 0);

    /* One timer keeps every status field live without per-event plumbing. */
    lv_timer_create(refresh_home, 1000, NULL);
    refresh_home(NULL);
}

/* ── WiFi tab ───────────────────────────────────────────────────── */
static void scan_worker(void *arg)
{
    (void)arg;

    wifi_ap_record_t *aps = calloc(SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    uint16_t          n   = SCAN_MAX_APS;

    if (aps == NULL || bsp_wifi_scan(aps, &n) != ESP_OK) n = 0;

    if (bsp_lvgl_lock(LOCK_TIMEOUT)) {
        lv_obj_clean(s_ap_list);

        if (n == 0) {
            lv_list_add_text(s_ap_list, "No networks found");
        } else {
            for (uint16_t i = 0; i < n; i++) {
                if (aps[i].ssid[0] == '\0') continue;   /* hidden */

                char label[80];
                snprintf(label, sizeof(label), "%s  (%d dBm)", aps[i].ssid, aps[i].rssi);

                /*
                 * Icon carries the most useful fact: a saved password beats
                 * knowing it is merely locked. The icon is a separate slot
                 * from the text, so on_ap_clicked's SSID parse is unaffected.
                 */
                const char *icon;
                if (bsp_wifi_store_has((const char *)aps[i].ssid)) {
                    icon = LV_SYMBOL_SAVE;
                } else if (aps[i].authmode == WIFI_AUTH_OPEN) {
                    icon = NULL;
                } else {
                    icon = LV_SYMBOL_CLOSE;
                }

                lv_obj_t *btn = lv_list_add_button(s_ap_list, icon, label);
                /*
                 * Per-button, not on the list: LVGL only propagates a child's
                 * event to its parent when the child has LV_OBJ_FLAG_EVENT_BUBBLE,
                 * which list buttons do not. A handler on the list alone never runs.
                 */
                lv_obj_add_event_cb(btn, on_ap_clicked, LV_EVENT_CLICKED, NULL);
            }
        }

        lv_label_set_text(s_lbl_wifi_status, n ? "Pick a network" : "Scan again?");
        lv_obj_remove_state(s_btn_scan, LV_STATE_DISABLED);
        bsp_lvgl_unlock();
    }

    free(aps);
    s_worker_busy = false;
    vTaskDelete(NULL);
}

static void on_scan_clicked(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;
    s_worker_busy = true;

    lv_obj_add_state(s_btn_scan, LV_STATE_DISABLED);
    lv_label_set_text(s_lbl_wifi_status, "Scanning...");

    if (xTaskCreate(scan_worker, "ui_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_worker_busy = false;
        lv_obj_remove_state(s_btn_scan, LV_STATE_DISABLED);
        lv_label_set_text(s_lbl_wifi_status, "Could not start scan");
    }
}

static void connect_worker(void *arg)
{
    char *pass = arg;

    /*
     * Drop the provisioning AP first. The radio is single-channel: while the
     * SoftAP holds channel 1, associating with a router on any other channel
     * needs a channel migration, and the C6 silently never completes it —
     * esp_wifi_connect() is accepted and no WiFi event ever comes back.
     * Once the user is setting WiFi up on the screen, the portal is redundant.
     */
    if (bsp_wifi_prov_is_active()) {
        set_text_locked(s_lbl_wifi_status, "Stopping setup access point...");
        bsp_wifi_prov_stop();
    }

    esp_err_t err = bsp_wifi_connect(s_sel_ssid, pass, false);
    if (err == ESP_OK && bsp_wifi_wait_connected(25000) == ESP_OK) {
        /* Save only once the credentials are proven, so a typo cannot
         * persist and make the next boot skip provisioning. */
        bsp_wifi_save_credentials(s_sel_ssid, pass);

        /* NVS holds only the most recent network, for auto-connect at boot;
         * the SD card keeps one password per SSID so any previously used
         * network can be picked again without retyping. */
        esp_err_t store_err = bsp_wifi_store_save(s_sel_ssid, pass);
        if (store_err == ESP_OK) {
            set_text_locked(s_lbl_wifi_status, "Connected - password saved to SD card");
        } else if (store_err == ESP_ERR_INVALID_STATE) {
            set_text_locked(s_lbl_wifi_status, "Connected (no SD card, password not saved)");
        } else {
            set_text_locked(s_lbl_wifi_status, "Connected (could not save to SD card)");
        }
    } else {
        set_text_locked(s_lbl_wifi_status, "Could not connect - check the password");
    }

    if (bsp_lvgl_lock(LOCK_TIMEOUT)) {
        lv_obj_remove_state(s_btn_connect, LV_STATE_DISABLED);
        bsp_lvgl_unlock();
    }

    free(pass);
    s_worker_busy = false;
    vTaskDelete(NULL);
}

static void on_connect_clicked(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;

    if (s_sel_ssid[0] == '\0') {
        lv_label_set_text(s_lbl_wifi_status, "Pick a network first");
        return;
    }

    /* Copied because the worker outlives this callback. */
    char *pass = strdup(lv_textarea_get_text(s_ta_pass));
    if (pass == NULL) return;

    s_worker_busy = true;
    lv_obj_add_state(s_btn_connect, LV_STATE_DISABLED);
    lv_label_set_text(s_lbl_wifi_status, "Connecting...");
    hide_keyboard();

    if (xTaskCreate(connect_worker, "ui_conn", 4096, pass, 4, NULL) != pdPASS) {
        free(pass);
        s_worker_busy = false;
        lv_obj_remove_state(s_btn_connect, LV_STATE_DISABLED);
        lv_label_set_text(s_lbl_wifi_status, "Could not start connection");
    }
}

/*
 * Raise the keyboard and keep the field it edits visible.
 *
 * The keyboard floats over the screen, so simply scrolling the field "into
 * view" is not enough: the tab's viewport still extends underneath it, and a
 * field scrolled to the bottom of that viewport sits behind the keys. The
 * content area has to actually shrink to the space above the keyboard, and
 * the layout has to be recomputed before scrolling, or the scroll is
 * calculated against the old geometry.
 */
static void show_keyboard_for(lv_obj_t *target)
{
    lv_keyboard_set_textarea(s_keyboard, target);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);

    lv_obj_set_height(s_tabs, LV_PCT(100 - KEYBOARD_PCT));
    lv_obj_update_layout(s_tabs);

    lv_obj_scroll_to_view(target, LV_ANIM_OFF);
}

/** Drop the keyboard and give the content area its full height back. */
static void hide_keyboard(void)
{
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_tabs, LV_PCT(100));
}

/** Attach to a text area so tapping it raises the keyboard for that field. */
static void on_textarea_clicked(lv_event_t *e)
{
    show_keyboard_for(lv_event_get_target(e));
}

static void on_ap_clicked(lv_event_t *e)
{
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *text = lv_list_get_button_text(s_ap_list, btn);
    if (text == NULL) return;

    /* The row reads "<ssid>  (-42 dBm)"; keep only the name. */
    const char *tail = strstr(text, "  (");
    size_t      len  = tail ? (size_t)(tail - text) : strlen(text);
    if (len > BSP_WIFI_SSID_MAX) len = BSP_WIFI_SSID_MAX;

    memcpy(s_sel_ssid, text, len);
    s_sel_ssid[len] = '\0';

    /*
     * If this network has been joined before, its password is on the SD card
     * — fill it in so it need not be retyped. Reading here blocks the LVGL
     * task briefly, but the file is a few hundred bytes at most.
     */
    char saved[BSP_WIFI_PASS_MAX + 1];
    if (bsp_wifi_store_lookup(s_sel_ssid, saved, sizeof(saved)) == ESP_OK) {
        lv_textarea_set_text(s_ta_pass, saved);
        set_text(s_lbl_wifi_status,
                 "Selected \"%s\" - saved password loaded, tap Connect", s_sel_ssid);
        /* Nothing to type, so leave the keyboard down. */
        hide_keyboard();
        return;
    }

    lv_textarea_set_text(s_ta_pass, "");
    set_text(s_lbl_wifi_status, "Selected \"%s\" - enter the password", s_sel_ssid);
    show_keyboard_for(s_ta_pass);
}

static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        hide_keyboard();
    }
}

static void build_wifi(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, PAD, 0);
    lv_obj_set_style_pad_row(tab, 8, 0);

    s_btn_scan = lv_button_create(tab);
    lv_obj_set_width(s_btn_scan, LV_PCT(100));
    lv_obj_add_event_cb(s_btn_scan, on_scan_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(s_btn_scan);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(scan_lbl);

    s_ap_list = lv_list_create(tab);
    lv_obj_set_width(s_ap_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ap_list, 1);
    lv_list_add_text(s_ap_list, "Tap Scan to list networks");

    s_ta_pass = lv_textarea_create(tab);
    lv_obj_set_width(s_ta_pass, LV_PCT(100));
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "Password");
    /* Tapping the field itself should also bring the keyboard up. */
    lv_obj_add_event_cb(s_ta_pass, on_textarea_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_wifi_status = lv_label_create(tab);
    lv_label_set_text(s_lbl_wifi_status, "Tap Scan to begin");
    lv_label_set_long_mode(s_lbl_wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_wifi_status, LV_PCT(100));

    s_btn_connect = lv_button_create(tab);
    lv_obj_set_width(s_btn_connect, LV_PCT(100));
    lv_obj_add_event_cb(s_btn_connect, on_connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *conn_lbl = lv_label_create(s_btn_connect);
    lv_label_set_text(conn_lbl, LV_SYMBOL_WIFI "  Connect");
    lv_obj_center(conn_lbl);

    /* Floats over the screen, so it belongs to the screen and not the tab. */
    s_keyboard = lv_keyboard_create(lv_screen_active());
    /* Same constant the content area is shrunk by, so the two always agree. */
    lv_obj_set_size(s_keyboard, LV_PCT(100), LV_PCT(KEYBOARD_PCT));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    hide_keyboard();
    lv_obj_add_event_cb(s_keyboard, on_keyboard_event, LV_EVENT_ALL, NULL);
}

/* ── Update tab ─────────────────────────────────────────────────── */
static void ota_progress_cb(size_t written, size_t total, void *user)
{
    (void)user;
    if (total == 0) return;

    int pct = (int)((uint64_t)written * 100 / total);

    /* Called from the update task, so the lock is required. Skip the frame
     * rather than stall the download if the UI is busy. */
    if (!bsp_lvgl_lock(20)) return;
    lv_bar_set_value(s_bar_ota, pct, LV_ANIM_OFF);
    char msg[48];
    snprintf(msg, sizeof(msg), "Downloading %d%%", pct);
    lv_label_set_text(s_lbl_ota, msg);
    bsp_lvgl_unlock();
}

static void ota_worker(void *arg)
{
    (void)arg;

    char        url[256] = {0};
    size_t      len      = sizeof(url);
    nvs_handle_t nvs;

    if (nvs_open("app_ota", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "url", url, &len) != ESP_OK) url[0] = '\0';
        nvs_close(nvs);
    }

    if (url[0] == '\0') {
        set_text_locked(s_lbl_ota, "No update URL set (NVS app_ota/url)");
    } else if (!bsp_wifi_is_connected()) {
        set_text_locked(s_lbl_ota, "Not connected to WiFi");
    } else {
        bsp_ota_config_t cfg = BSP_OTA_CONFIG_DEFAULT();
        cfg.on_progress = ota_progress_cb;

        /* Reboots on success and never returns. */
        esp_err_t err = bsp_ota_update_from_url(url, &cfg);
        if (err == ESP_ERR_INVALID_VERSION) {
            set_text_locked(s_lbl_ota, "Already up to date");
        } else {
            set_text_locked(s_lbl_ota, esp_err_to_name(err));
        }
    }

    if (bsp_lvgl_lock(LOCK_TIMEOUT)) {
        lv_obj_remove_state(s_btn_ota, LV_STATE_DISABLED);
        bsp_lvgl_unlock();
    }

    s_worker_busy = false;
    vTaskDelete(NULL);
}

static void on_ota_clicked(lv_event_t *e)
{
    (void)e;
    if (s_worker_busy) return;
    s_worker_busy = true;

    lv_obj_add_state(s_btn_ota, LV_STATE_DISABLED);
    lv_bar_set_value(s_bar_ota, 0, LV_ANIM_OFF);
    lv_label_set_text(s_lbl_ota, "Checking...");

    if (xTaskCreate(ota_worker, "ui_ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_worker_busy = false;
        lv_obj_remove_state(s_btn_ota, LV_STATE_DISABLED);
        lv_label_set_text(s_lbl_ota, "Could not start update");
    }
}

static void build_update(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, PAD, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "Firmware update");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    s_btn_ota = lv_button_create(tab);
    lv_obj_set_width(s_btn_ota, LV_PCT(100));
    lv_obj_add_event_cb(s_btn_ota, on_ota_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ota_lbl = lv_label_create(s_btn_ota);
    lv_label_set_text(ota_lbl, LV_SYMBOL_DOWNLOAD "  Check for update");
    lv_obj_center(ota_lbl);

    s_bar_ota = lv_bar_create(tab);
    lv_obj_set_width(s_bar_ota, LV_PCT(100));
    lv_bar_set_range(s_bar_ota, 0, 100);
    lv_bar_set_value(s_bar_ota, 0, LV_ANIM_OFF);

    s_lbl_ota = lv_label_create(tab);
    lv_label_set_text(s_lbl_ota, "Idle");
    lv_label_set_long_mode(s_lbl_ota, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_ota, LV_PCT(100));

    lv_obj_t *card = make_card(tab);
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Or upload a .bin from a browser:");
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    s_lbl_upload_url = lv_label_create(card);
    lv_label_set_text(s_lbl_upload_url, "connect to WiFi first");

    lv_obj_t *sd = lv_label_create(card);
    lv_label_set_text(sd, "Or place firmware.bin on the SD card and reboot.");
    lv_label_set_long_mode(sd, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sd, LV_PCT(100));
    lv_obj_set_style_text_opa(sd, LV_OPA_70, 0);
}

/* ── Control tab ────────────────────────────────────────────────── */
/*
 * The mirror image of the web page's Control tab: both edit the same
 * bsp_remote state, so a change in either place shows up in the other.
 *
 * s_applying guards against a feedback loop — while widgets are being set
 * from an incoming change, their event handlers must not push that same
 * value straight back out again.
 */
static lv_obj_t *s_ctl_mode;
static lv_obj_t *s_ctl_level;
static lv_obj_t *s_ctl_level_val;
static lv_obj_t *s_ctl_led;
static lv_obj_t *s_ctl_label;
static lv_obj_t *s_ctl_source;
static bool      s_applying;

/** Push the widgets' current values into the shared state. */
static void publish_from_widgets(void)
{
    if (s_applying) return;

    bsp_remote_state_t s;
    bsp_remote_get(&s);

    s.mode  = (bsp_remote_mode_t)lv_dropdown_get_selected(s_ctl_mode);
    s.level = lv_slider_get_value(s_ctl_level);
    s.led   = lv_obj_has_state(s_ctl_led, LV_STATE_CHECKED);

    lv_label_set_text(s_ctl_source, "Changed on the device");
    bsp_remote_set(&s);
}

static void on_control_changed(lv_event_t *e)
{
    (void)e;
    publish_from_widgets();
}

/** Paint incoming state onto the widgets. Must hold the LVGL lock. */
static void apply_state_to_widgets(const bsp_remote_state_t *s)
{
    s_applying = true;

    lv_dropdown_set_selected(s_ctl_mode, (uint16_t)s->mode);
    lv_slider_set_value(s_ctl_level, s->level, LV_ANIM_OFF);
    set_text(s_ctl_level_val, "%d", s->level);

    if (s->led) lv_obj_add_state(s_ctl_led, LV_STATE_CHECKED);
    else        lv_obj_remove_state(s_ctl_led, LV_STATE_CHECKED);

    lv_label_set_text(s_ctl_label, s->label[0] ? s->label : "(no message)");

    s_applying = false;
}

/*
 * Called by bsp_remote on whichever task made the change — the HTTP server
 * task for a browser edit, or the LVGL task for an on-screen one. The lock is
 * recursive, so re-taking it on the LVGL task is harmless; s_applying is here
 * to stop the feedback loop, not a deadlock: repainting a widget fires its
 * VALUE_CHANGED handler, which would publish the same value straight back.
 */
static void on_remote_changed(const bsp_remote_state_t *state, void *user)
{
    (void)user;

    if (s_applying) return;   /* our own write echoing back */

    if (!bsp_lvgl_lock(LOCK_TIMEOUT)) return;
    apply_state_to_widgets(state);
    lv_label_set_text(s_ctl_source, "Changed from the web page");
    bsp_lvgl_unlock();
}

static void build_control(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, PAD, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "Control");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "Shared with the web page — change it in either place.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);

    lv_obj_t *card = make_card(tab);

    /* Mode — a dropdown rather than three radio buttons: same one-of-N
     * choice, far easier to hit on a 480 px-wide touchscreen. */
    lv_obj_t *mode_row = lv_label_create(card);
    lv_label_set_text(mode_row, "Mode");
    lv_obj_set_style_text_opa(mode_row, LV_OPA_70, 0);

    s_ctl_mode = lv_dropdown_create(card);
    lv_dropdown_set_options(s_ctl_mode, "Idle\nRun\nTest");
    lv_obj_set_width(s_ctl_mode, LV_PCT(100));
    lv_obj_add_event_cb(s_ctl_mode, on_control_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_ctl_level_val = make_row(card, "Level");

    s_ctl_level = lv_slider_create(card);
    lv_slider_set_range(s_ctl_level, 0, 100);
    lv_obj_set_width(s_ctl_level, LV_PCT(100));
    lv_obj_add_event_cb(s_ctl_level, on_control_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *led_row = lv_obj_create(card);
    lv_obj_set_width(led_row, LV_PCT(100));
    lv_obj_set_height(led_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(led_row, 0, 0);
    lv_obj_set_style_border_width(led_row, 0, 0);
    lv_obj_set_style_bg_opa(led_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(led_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(led_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(led_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *led_lbl = lv_label_create(led_row);
    lv_label_set_text(led_lbl, "Indicator");
    lv_obj_set_style_text_opa(led_lbl, LV_OPA_70, 0);

    s_ctl_led = lv_switch_create(led_row);
    lv_obj_add_event_cb(s_ctl_led, on_control_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *msg_card = make_card(tab);
    lv_obj_t *msg_hdr  = lv_label_create(msg_card);
    lv_label_set_text(msg_hdr, "Message");
    lv_obj_set_style_text_opa(msg_hdr, LV_OPA_70, 0);

    s_ctl_label = lv_label_create(msg_card);
    lv_label_set_long_mode(s_ctl_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ctl_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_ctl_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ctl_label, "(no message)");

    s_ctl_source = lv_label_create(tab);
    lv_label_set_text(s_ctl_source, "Waiting for a change...");
    lv_obj_set_style_text_opa(s_ctl_source, LV_OPA_60, 0);

    /* Seed the widgets, then follow every later change. */
    bsp_remote_state_t s;
    bsp_remote_get(&s);
    apply_state_to_widgets(&s);
    bsp_remote_subscribe(on_remote_changed, NULL);
}

/* ── BLE monitor tab ────────────────────────────────────────────── */
/*
 * A rolling window of the last few exchanges over the BLE UART.
 *
 * Kept as a fixed ring of short lines rather than appending to the text area
 * forever: an unbounded log would grow until LVGL slowed to a crawl, and the
 * interesting traffic is always the most recent.
 */
#define LOG_LINES    18
#define LOG_LINE_LEN 56
/* Room for the ">> " / "<< " prefix and the terminator. */
#define LOG_BODY_LEN (LOG_LINE_LEN - 4)

static lv_obj_t *s_ble_log;
static lv_obj_t *s_lbl_ble_conn;
static lv_obj_t *s_ta_ble_send;
static char      s_log[LOG_LINES][LOG_LINE_LEN];
static int       s_log_count;

/** Render printable characters, and make the rest visible rather than absent. */
static void escape_into(char *dst, size_t dst_len, const char *src, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c >= 0x20 && c < 0x7f) { dst[o++] = (char)c; }
        else { o += snprintf(dst + o, dst_len - o, "\\x%02x", c); }
    }
    dst[o] = '\0';
}

/** Rebuild the text area from the ring. Caller holds the LVGL lock. */
static void repaint_log(void)
{
    /*
     * Static, not on the stack. This runs on whichever task logged — usually
     * the NimBLE host task, whose stack is small and already carries the BLE
     * call chain plus reply() and set_text buffers. A kilobyte here overflowed
     * it: "Guru Meditation Error: Core 0 panic'ed (Stack protection fault)".
     * Safe as a single instance because every caller holds the LVGL lock.
     */
    static char buf[LOG_LINES * LOG_LINE_LEN];
    size_t o = 0;

    for (int i = 0; i < s_log_count && o < sizeof(buf) - 2; i++) {
        int n = snprintf(buf + o, sizeof(buf) - o, "%s\n", s_log[i]);
        if (n < 0) break;
        o += (size_t)n;
    }
    buf[o] = '\0';

    lv_textarea_set_text(s_ble_log, buf);
    /* Keep the newest line in view. */
    lv_obj_scroll_to_y(s_ble_log, LV_COORD_MAX, LV_ANIM_OFF);
}

/** Append one entry. Caller holds the LVGL lock. */
static void log_push(const char *text, size_t len, bool incoming)
{
    char body[LOG_BODY_LEN];
    escape_into(body, sizeof(body), text, len);
    if (body[0] == '\0') return;

    if (s_log_count == LOG_LINES) {
        /* Drop the oldest; cheap enough at this size. */
        memmove(s_log[0], s_log[1], sizeof(s_log) - sizeof(s_log[0]));
        s_log_count--;
    }
    snprintf(s_log[s_log_count], LOG_LINE_LEN, "%s %s",
             incoming ? ">>" : "<<", body);
    s_log_count++;
}

void ui_ble_log(const char *text, size_t len, bool incoming)
{
    if (s_ble_log == NULL || text == NULL || len == 0) return;

    if (!bsp_lvgl_lock(LOCK_TIMEOUT)) return;

    /*
     * Split on newlines so a multi-line reply becomes several entries rather
     * than one truncated blob. Text with no newline at all still yields one
     * entry, which is what makes unterminated input visible.
     */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t seg = i - start;
            /* Trim a trailing CR so CRLF does not leave a stray marker. */
            if (seg > 0 && text[start + seg - 1] == '\r') seg--;
            if (seg > 0) log_push(text + start, seg, incoming);
            start = i + 1;
        }
    }

    repaint_log();
    bsp_lvgl_unlock();
}

static void on_log_clear(lv_event_t *e)
{
    (void)e;
    s_log_count = 0;
    repaint_log();
}

/*
 * Hand the text to the console's queue and return. Transmitting here would
 * mean calling NimBLE while holding the LVGL lock, which is the reverse of the
 * order the receive path takes them in — see ble_console_send().
 */
static void on_ble_send(lv_event_t *e)
{
    (void)e;

    const char *text = lv_textarea_get_text(s_ta_ble_send);
    if (text == NULL || text[0] == '\0') return;

    ble_console_send(text);
    lv_textarea_set_text(s_ta_ble_send, "");
    hide_keyboard();
}

static void refresh_ble_conn(lv_timer_t *timer)
{
    (void)timer;

    const char *state = bsp_ble_is_connected()   ? "central connected"
                      : bsp_ble_is_advertising() ? "advertising as \"gution-p4\""
                      : bsp_ble_is_ready()       ? "idle"
                                                 : "unavailable";
    set_text(s_lbl_ble_conn, "%s", state);
}

static void build_ble(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, PAD, 0);
    lv_obj_set_style_pad_row(tab, 8, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "BLE monitor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    s_lbl_ble_conn = lv_label_create(tab);
    lv_label_set_text(s_lbl_ble_conn, "...");
    lv_obj_set_style_text_opa(s_lbl_ble_conn, LV_OPA_60, 0);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, ">> from the phone,  << replies. Try: help");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);

    s_ble_log = lv_textarea_create(tab);
    lv_obj_set_width(s_ble_log, LV_PCT(100));
    lv_obj_set_flex_grow(s_ble_log, 1);
    lv_textarea_set_placeholder_text(s_ble_log, "Nothing received yet");
    /* Read-only: this is a monitor, and a cursor would invite the keyboard. */
    lv_obj_remove_flag(s_ble_log, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_ble_log, &lv_font_montserrat_14, 0);
    lv_textarea_set_text(s_ble_log, "");

    /* Device -> central: type here and it appears in the phone's terminal. */
    s_ta_ble_send = lv_textarea_create(tab);
    lv_obj_set_width(s_ta_ble_send, LV_PCT(100));
    lv_textarea_set_one_line(s_ta_ble_send, true);
    lv_textarea_set_placeholder_text(s_ta_ble_send, "Message to send");
    lv_obj_add_event_cb(s_ta_ble_send, on_textarea_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ta_ble_send, on_ble_send, LV_EVENT_READY, NULL);

    lv_obj_t *row = lv_obj_create(tab);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *send = lv_button_create(row);
    lv_obj_set_flex_grow(send, 1);
    lv_obj_add_event_cb(send, on_ble_send, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, LV_SYMBOL_UPLOAD "  Send");
    lv_obj_center(send_lbl);

    lv_obj_t *clear = lv_button_create(row);
    lv_obj_set_flex_grow(clear, 1);
    lv_obj_add_event_cb(clear, on_log_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, LV_SYMBOL_TRASH "  Clear");
    lv_obj_center(clear_lbl);

    lv_timer_create(refresh_ble_conn, 1000, NULL);
    refresh_ble_conn(NULL);
}

/* ── Remote control of the view ─────────────────────────────────── */
void ui_select_tab(ui_tab_t tab)
{
    if (s_tabs == NULL || tab < 0 || tab >= UI_TAB_COUNT) return;

    if (!bsp_lvgl_lock(LOCK_TIMEOUT)) return;
    lv_tabview_set_active(s_tabs, (uint32_t)tab, LV_ANIM_OFF);
    bsp_lvgl_unlock();
}

/* ── Entry point ────────────────────────────────────────────────── */
esp_err_t ui_init(void)
{
    if (!bsp_lvgl_lock(LOCK_TIMEOUT)) {
        ESP_LOGE(TAG, "could not take the LVGL lock");
        return ESP_FAIL;
    }

    s_tabs = lv_tabview_create(lv_screen_active());
    lv_obj_t *tabs = s_tabs;
    lv_tabview_set_tab_bar_size(tabs, 56);

    build_home(lv_tabview_add_tab(tabs, "Home"));
    build_control(lv_tabview_add_tab(tabs, "Control"));
    build_ble(lv_tabview_add_tab(tabs, "BLE"));
    build_wifi(lv_tabview_add_tab(tabs, "WiFi"));
    build_update(lv_tabview_add_tab(tabs, "Update"));

    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}
