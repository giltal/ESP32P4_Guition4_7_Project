/*
 * WiFi station — radio lives on the ESP32-C6 via ESP-Hosted.
 */

#include "bsp_wifi.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "bsp_wifi";

#define NVS_NAMESPACE   "bsp_wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define CONNECTED_BIT   BIT0

/* Retry backoff: double on each failure, capped. */
#define RETRY_BASE_MS   1000
#define RETRY_MAX_MS    30000

/*
 * Liveness checks, on two planes.
 *
 * The C6 does not always report a lost link: the association can die with no
 * WIFI_EVENT_STA_DISCONNECTED ever arriving, leaving the host convinced it is
 * online. So instead of trusting the absence of a disconnect, verify.
 *
 * Two checks are needed because they fail independently:
 *
 *  - Control plane: esp_wifi_sta_get_ap_info(), an RPC to the C6. Proves the
 *    RPC channel works and the C6 still thinks it is associated.
 *  - Data plane: ping the gateway. Proves packets actually move.
 *
 * Observed on this board: the control-plane check kept passing — associated,
 * RSSI readable, IP held — while nothing could reach the device at all. Only
 * the data-plane check catches that.
 */
#define LINK_CHECK_MS   15000
#define LINK_FAIL_LIMIT 3       /* ~45 s of failures before forcing a reconnect */
#define PING_COUNT      3
#define PING_TIMEOUT_MS 1000
#define PING_WAIT_MS    3000

static bool                 s_started;
static bool                 s_auto_retry;
static bsp_wifi_state_t     s_state = BSP_WIFI_STOPPED;
static EventGroupHandle_t   s_events;
static esp_netif_t         *s_netif;
static esp_timer_handle_t   s_retry_timer;
static uint32_t             s_retry_delay_ms = RETRY_BASE_MS;
static esp_ip4_addr_t       s_ip;

/* Last credentials used, so the link monitor can reconnect without NVS. */
static char                 s_ssid[BSP_WIFI_SSID_MAX + 1];
static char                 s_pass[BSP_WIFI_PASS_MAX + 1];

/* RSSI is sampled by the link monitor; readers get the cached value so that
 * no caller ends up blocking on an RPC to the C6. */
static volatile int         s_rssi;
static volatile bool        s_rssi_valid;
static volatile uint32_t    s_link_recoveries;

/* ── Retry ──────────────────────────────────────────────────────── */
/*
 * Reconnect from a timer callback rather than sleeping inside the event
 * handler — the event loop task is shared, and blocking it stalls every
 * other subsystem's events too.
 */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_auto_retry) return;
    ESP_LOGI(TAG, "Retrying connection");
    s_state = BSP_WIFI_CONNECTING;
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    if (!s_auto_retry || !s_retry_timer) return;

    esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "Reconnecting in %" PRIu32 " ms", s_retry_delay_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_delay_ms * 1000);

    s_retry_delay_ms *= 2;
    if (s_retry_delay_ms > RETRY_MAX_MS) s_retry_delay_ms = RETRY_MAX_MS;
}

/* ── Events ─────────────────────────────────────────────────────── */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Station started");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Associated — waiting for IP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Disconnected from \"%.*s\" (reason %d)",
                 d->ssid_len, (const char *)d->ssid, d->reason);
        s_state = BSP_WIFI_DISCONNECTED;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        memset(&s_ip, 0, sizeof(s_ip));
        schedule_retry();
        break;
    }

    default:
        /* Logged so a silent radio is distinguishable from an unhandled
         * event — a connect that produces no event at all means the request
         * never came back from the C6. */
        ESP_LOGI(TAG, "WiFi event %" PRId32, id);
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *evt = data;
    s_ip = evt->ip_info.ip;
    ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&s_ip));

    /* Connection is good — reset backoff so the next drop retries promptly. */
    s_retry_delay_ms = RETRY_BASE_MS;
    s_state = BSP_WIFI_CONNECTED;
    xEventGroupSetBits(s_events, CONNECTED_BIT);
}

/* ── Data-plane check ───────────────────────────────────────────── */
static volatile bool s_ping_replied;
static volatile bool s_ping_ever_replied;

static void on_ping_reply(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    s_ping_replied      = true;
    s_ping_ever_replied = true;
}

/**
 * @return true when the gateway answered, or when this network has never
 *         answered ICMP at all — some routers drop it, and treating that as a
 *         dead link would put the device in a permanent reconnect loop.
 */
static bool gateway_reachable(void)
{
    esp_netif_ip_info_t info;
    if (s_netif == NULL || esp_netif_get_ip_info(s_netif, &info) != ESP_OK) return true;
    if (info.gw.addr == 0) return true;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type          = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = info.gw.addr;
    cfg.count       = PING_COUNT;
    cfg.timeout_ms  = PING_TIMEOUT_MS;
    cfg.interval_ms = 200;

    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_reply };
    esp_ping_handle_t    ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) return true;

    s_ping_replied = false;
    esp_ping_start(ping);
    vTaskDelay(pdMS_TO_TICKS(PING_WAIT_MS));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);

    if (s_ping_replied) return true;

    /* Never seen a reply on this network: assume ICMP is filtered rather than
     * that the link is broken. */
    return !s_ping_ever_replied;
}

/* ── Link monitor ───────────────────────────────────────────────── */
/*
 * Verifies that "connected" is still true rather than merely un-contradicted,
 * and samples RSSI while it is there. Runs on its own task so the blocking
 * RPC to the C6 can stall without taking anything else down with it.
 */
static void link_monitor_task(void *arg)
{
    (void)arg;
    int fails = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_MS));

        if (s_state != BSP_WIFI_CONNECTED) {
            fails        = 0;
            s_rssi_valid = false;
            continue;
        }

        wifi_ap_record_t ap;
        bool associated = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (associated) {
            s_rssi       = ap.rssi;
            s_rssi_valid = true;
        } else {
            s_rssi_valid = false;
        }

        /* Only worth pinging if the C6 still claims an association. */
        bool passing = associated && gateway_reachable();
        if (passing) {
            fails = 0;
            continue;
        }

        if (++fails < LINK_FAIL_LIMIT) {
            ESP_LOGW(TAG, "Link check failed (%d/%d) — associated=%s",
                     fails, LINK_FAIL_LIMIT, associated ? "yes" : "no");
            continue;
        }

        ESP_LOGW(TAG, "Link dead (associated=%s, gateway unreachable) — reconnecting",
                 associated ? "yes" : "no");
        fails = 0;
        s_link_recoveries++;

        /* Drop our own idea of the state first, so anything waiting on
         * bsp_wifi_wait_connected() is not told we are online. */
        s_state = BSP_WIFI_DISCONNECTED;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        memset(&s_ip, 0, sizeof(s_ip));

        if (s_ssid[0]) {
            /* Full stop/start resync — the same treatment that gets the
             * hosted state machine unstuck after a mode change. */
            bsp_wifi_connect(s_ssid, s_pass, false);
        } else {
            ESP_LOGE(TAG, "No credentials held in RAM — cannot reconnect");
        }
    }
}

uint32_t bsp_wifi_link_recoveries(void)
{
    return s_link_recoveries;
}

/* ── Init ───────────────────────────────────────────────────────── */
esp_err_t bsp_wifi_init(void)
{
    if (s_started) return ESP_OK;

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");

    /* Tolerate an event loop the application already created. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "create sta netif");

    /* Opens the SDIO link and boots the C6's hosted slave. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_ip_event, NULL, NULL),
        TAG, "ip handler");

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_started = true;
    s_state   = BSP_WIFI_DISCONNECTED;

    xTaskCreate(link_monitor_task, "wifi_link", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "WiFi ready (radio on ESP32-C6 via ESP-Hosted)");
    return ESP_OK;
}

/* ── Credentials ────────────────────────────────────────────────── */
static esp_err_t creds_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");

    esp_err_t err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_PASS, password ? password : "");
    if (err == ESP_OK) err = nvs_commit(nvs);

    nvs_close(nvs);
    return err;
}

esp_err_t bsp_wifi_save_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "empty SSID");
    return creds_save(ssid, password);
}

bool bsp_wifi_has_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t    len = 0;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_SSID, NULL, &len);
    nvs_close(nvs);

    /* len counts the terminator, so a stored empty string is still "none". */
    return err == ESP_OK && len > 1;
}

esp_err_t bsp_wifi_forget(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    esp_err_t err = nvs_commit(nvs);

    nvs_close(nvs);
    return err;
}

esp_err_t bsp_wifi_connect_saved(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No stored credentials");
        return ESP_ERR_NOT_FOUND;
    }

    char   ssid[BSP_WIFI_SSID_MAX + 1] = {0};
    char   pass[BSP_WIFI_PASS_MAX + 1] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        /* A missing password is fine — it means an open network. */
        if (nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len) != ESP_OK) pass[0] = '\0';
    }
    nvs_close(nvs);

    if (err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "No stored credentials");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Using stored credentials for \"%s\"", ssid);
    return bsp_wifi_connect(ssid, pass, false);
}

/* ── Connect / disconnect ───────────────────────────────────────── */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password, bool save)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "empty SSID");
    ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "init");

    bool open_network = (password == NULL || password[0] == '\0');

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (!open_network) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    /* WPA_PSK rather than WPA2_PSK: some APs still advertise mixed mode. */
    cfg.sta.threshold.authmode = open_network ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");

    if (save) {
        esp_err_t err = creds_save(ssid, password);
        if (err != ESP_OK) ESP_LOGW(TAG, "Could not store credentials: %s", esp_err_to_name(err));
    }

    /* Kept so the link monitor can reconnect without going back to NVS. */
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, open_network ? "" : password, sizeof(s_pass));

    s_auto_retry     = true;
    s_retry_delay_ms = RETRY_BASE_MS;
    s_state          = BSP_WIFI_CONNECTING;
    xEventGroupClearBits(s_events, CONNECTED_BIT);

    /*
     * Resynchronise ESP-Hosted's netif latches before associating.
     *
     * rpc_wrap.c tracks `netif_started`/`netif_connected` and forwards the
     * slave's StaConnected to the host event loop only when
     * `!netif_connected && netif_started`. `netif_started` is latched on
     * STA_START, but *only* if the STA netif is not already up:
     *
     *     if (!netif_started && !is_wifi_netif_started(WIFI_IF_STA)) { ... }
     *
     * Lose that race — as happens after a mode change such as leaving
     * AP+STA — and the flag stays false permanently. The association then
     * succeeds on the C6 and is logged there, but WIFI_EVENT_STA_CONNECTED
     * never reaches esp_netif, so DHCP never starts and no IP ever arrives:
     * a connect that neither succeeds nor reports an error.
     *
     * WIFI_EVENT_STA_STOP clears both latches unconditionally, so a
     * stop/start cycle puts the state machine back in a known state.
     */
    esp_wifi_stop();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "re-set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "restart");
    /* Give STA_START time to travel back from the C6 and set the latch. */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Connecting to \"%s\"%s", ssid, open_network ? " (open)" : "");

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        s_state = BSP_WIFI_DISCONNECTED;
    }
    return err;
}

esp_err_t bsp_wifi_disconnect(void)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;

    /* Clear the retry flag first, or the disconnect event reschedules. */
    s_auto_retry = false;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);

    esp_err_t err = esp_wifi_disconnect();
    s_state = BSP_WIFI_DISCONNECTED;
    xEventGroupClearBits(s_events, CONNECTED_BIT);
    return err;
}

/* ── Status ─────────────────────────────────────────────────────── */
esp_err_t bsp_wifi_wait_connected(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bsp_wifi_state_t bsp_wifi_state(void)
{
    return s_state;
}

bool bsp_wifi_is_connected(void)
{
    return s_state == BSP_WIFI_CONNECTED;
}

esp_err_t bsp_wifi_get_ip(esp_ip4_addr_t *out_ip)
{
    ESP_RETURN_ON_FALSE(out_ip, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(bsp_wifi_is_connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");
    *out_ip = s_ip;
    return ESP_OK;
}

esp_err_t bsp_wifi_get_rssi(int *out_rssi)
{
    if (out_rssi == NULL) return ESP_ERR_INVALID_ARG;

    /* Deliberately non-blocking: returns the sample taken by the link
     * monitor rather than issuing an RPC. Callers include LVGL timers, which
     * hold the UI lock and must never wait on the radio. */
    if (!s_rssi_valid) return ESP_ERR_INVALID_STATE;

    *out_rssi = s_rssi;
    return ESP_OK;
}

esp_err_t bsp_wifi_scan(wifi_ap_record_t *out_records, uint16_t *io_count)
{
    ESP_RETURN_ON_FALSE(out_records && io_count && *io_count,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "init");

    ESP_LOGI(TAG, "Scanning");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan");

    esp_err_t err = esp_wifi_scan_get_ap_records(io_count, out_records);
    if (err != ESP_OK) {
        /* Always release the driver's result buffer, even on failure. */
        esp_wifi_scan_stop();
        *io_count = 0;
        return err;
    }

    ESP_LOGI(TAG, "Found %u access points", *io_count);
    return ESP_OK;
}
