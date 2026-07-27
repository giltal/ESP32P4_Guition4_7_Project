/*
 * Line-oriented console over the BLE UART — see ble_console.h.
 */

#include "ble_console.h"
#include "ble_uart.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>

#include "bsp_remote.h"
#include "bsp_wifi.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "ble_con";

/* Terminal apps send a line at a time, but nothing guarantees it arrives in
 * one write — so accumulate until a newline. */
#define LINE_MAX 96

static char   s_line[LINE_MAX];
static size_t s_len;

/** Send to the central and mirror onto the device's BLE monitor tab. */
static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void reply(const char *fmt, ...)
{
    char    buf[288];
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    ble_uart_send(buf, (size_t)n);
    ui_ble_log(buf, (size_t)n, false);
}

/* ── Commands ───────────────────────────────────────────────────── */
/*
 * Built into one buffer and sent once. Seven separate reply() calls meant
 * seven notifications per `status`, and a client issuing them back to back
 * could exhaust NimBLE's mbuf pool and drop the connection.
 */
static void cmd_status(void)
{
    bsp_remote_state_t s;
    bsp_remote_get(&s);

    char ip_text[24] = "offline";
    esp_ip4_addr_t ip;
    if (bsp_wifi_get_ip(&ip) == ESP_OK) {
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip));
    }

    reply("mode  %d (%s)\r\n"
          "level %d\r\n"
          "led   %s\r\n"
          "msg   %s\r\n"
          "wifi  %s\r\n"
          "fw    %s\r\n",
          (int)s.mode, bsp_remote_mode_name(s.mode),
          s.level,
          s.led ? "on" : "off",
          s.label,
          ip_text,
          esp_app_get_description()->version);
}

static void cmd_help(void)
{
    reply(
        "commands:\r\n"
        "  status         show everything\r\n"
        "  mode <0|1|2>   Idle / Run / Test\r\n"
        "  level <0-100>\r\n"
        "  led <on|off>\r\n"
        "  msg <text>     shown on the device\r\n"
        "  tab <0-4>      switch the screen\r\n"
        "  help\r\n");
}

/** @return true when the argument parsed and was applied. */
static bool apply(const char *cmd, const char *arg)
{
    bsp_remote_state_t s;
    bsp_remote_get(&s);

    if (strcmp(cmd, "mode") == 0) {
        int v = atoi(arg);
        if (v < 0 || v >= BSP_REMOTE_MODE_COUNT) {
            reply("mode must be 0..%d\r\n", BSP_REMOTE_MODE_COUNT - 1);
            return false;
        }
        s.mode = (bsp_remote_mode_t)v;

    } else if (strcmp(cmd, "level") == 0) {
        int v = atoi(arg);
        if (v < 0 || v > 100) {
            reply("level must be 0..100\r\n");
            return false;
        }
        s.level = v;

    } else if (strcmp(cmd, "led") == 0) {
        if      (strcmp(arg, "on") == 0  || strcmp(arg, "1") == 0) s.led = true;
        else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) s.led = false;
        else { reply("led takes on or off\r\n"); return false; }

    } else if (strcmp(cmd, "msg") == 0) {
        strlcpy(s.label, arg, sizeof(s.label));

    } else if (strcmp(cmd, "tab") == 0) {
        int v = atoi(arg);
        if (v < 0 || v >= UI_TAB_COUNT) {
            reply("tab must be 0..%d\r\n", UI_TAB_COUNT - 1);
            return false;
        }
        /* Only changes what is on screen — no shared state to publish. */
        ui_select_tab((ui_tab_t)v);
        reply("ok\r\n");
        return true;

    } else {
        return false;
    }

    /* Reaches the display and the web page as well. */
    bsp_remote_set(&s);
    reply("ok\r\n");
    return true;
}

static void handle_line(char *line)
{
    while (*line == ' ') line++;
    if (*line == '\0') return;

    /* Split into command and the rest — "msg" wants the remainder verbatim,
     * spaces included. */
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    } else {
        arg = "";
    }

    for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        cmd_help();
    } else if (strcmp(line, "status") == 0) {
        cmd_status();
    } else if (!apply(line, arg)) {
        reply("unknown: %s (try help)\r\n", line);
    }
}

/* ── Receive ────────────────────────────────────────────────────── */
static void on_rx(const uint8_t *data, size_t len, void *user)
{
    (void)user;

    /*
     * Show the raw chunk before any line assembly. A client that never sends a
     * newline would otherwise produce no output at all — no echo, no reply —
     * and look exactly like one that never connected.
     */
    ui_ble_log((const char *)data, len, true);

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\r' || c == '\n') {
            if (s_len > 0) {
                s_line[s_len] = '\0';
                s_len = 0;
                handle_line(s_line);
            }
            continue;
        }

        if (s_len + 1 < sizeof(s_line)) {
            s_line[s_len++] = c;
        } else {
            /* Overlong line: drop it rather than act on a truncated command. */
            s_len = 0;
            reply("line too long\r\n");
        }
    }
}

/* ── Device-initiated messages ──────────────────────────────────── */
/*
 * Outgoing text is queued and transmitted by a task of its own.
 *
 * The receive path runs on the NimBLE host task and takes the LVGL lock (to
 * draw on the monitor tab) while holding NimBLE's. Sending straight from a
 * button handler would do the reverse — LVGL lock held, NimBLE lock wanted —
 * and two paths taking the same locks in opposite orders deadlock. Here the
 * worker holds neither when it starts, and takes them one after the other.
 */
#define TX_QUEUE_DEPTH 8
#define TX_TEXT_MAX    96

static QueueHandle_t s_tx_queue;

static void tx_task(void *arg)
{
    (void)arg;

    char text[TX_TEXT_MAX];
    for (;;) {
        if (xQueueReceive(s_tx_queue, text, portMAX_DELAY) != pdTRUE) continue;

        char line[TX_TEXT_MAX + 2];
        int n = snprintf(line, sizeof(line), "%s\r\n", text);
        if (n <= 0) continue;

        /* NimBLE first, then LVGL — never both at once. */
        if (ble_uart_send(line, (size_t)n) == ESP_OK) {
            ui_ble_log(line, (size_t)n, false);
        } else {
            ui_ble_log("(not sent - nobody connected)", 28, false);
        }
    }
}

void ble_console_send(const char *text)
{
    if (s_tx_queue == NULL || text == NULL || text[0] == '\0') return;

    char buf[TX_TEXT_MAX];
    strlcpy(buf, text, sizeof(buf));

    /* Never block: this is called from LVGL handlers, which must not wait. */
    if (xQueueSend(s_tx_queue, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full — message dropped");
    }
}

void ble_console_start(void)
{
    ble_uart_set_rx_handler(on_rx, NULL);

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, TX_TEXT_MAX);
    if (s_tx_queue == NULL ||
        xTaskCreate(tx_task, "ble_tx", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the send path");
        s_tx_queue = NULL;
    }

    ESP_LOGI(TAG, "Console attached to the BLE UART");
}
