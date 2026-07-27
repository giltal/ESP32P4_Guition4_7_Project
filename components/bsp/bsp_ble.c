/*
 * BLE peripheral — NimBLE host on the P4, controller on the C6 over ESP-Hosted.
 */

#include "bsp_ble.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "bsp_ble";

#define DEFAULT_DEVICE_NAME  "gution-p4"
#define DEFAULT_ADV_INTERVAL 100  /* ms */

/*
 * Several centrals may be connected at once — a phone and a PC, typically.
 * NimBLE stops advertising the moment one connects, so advertising has to be
 * restarted explicitly while there is still capacity; otherwise the second
 * central sees a stale scan result and fails to connect to a device that is no
 * longer advertising.
 */
#define MAX_CONN CONFIG_BT_NIMBLE_MAX_CONNECTIONS

static bool              s_running;
static bool              s_synced;      /* controller answered and is usable */
static bool              s_advertising;
static bool              s_connected;
static uint16_t          s_conns[MAX_CONN];
static int               s_conn_count;
static uint16_t          s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t           s_addr_type;
static uint32_t          s_adv_interval_ms = DEFAULT_ADV_INTERVAL;
static bsp_ble_conn_cb_t s_conn_cb;
static bool              s_have_adv_uuid;
static ble_uuid128_t     s_adv_uuid;
static bool              s_require_pairing;

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* ── Advertising ────────────────────────────────────────────────── */
esp_err_t bsp_ble_advertise_start(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;

    const char *name = ble_svc_gap_device_name();

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* Advertise our TX power so centrals can estimate range. */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                  = (uint8_t *)name;
    fields.name_len              = strlen(name);
    fields.name_is_complete      = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    /*
     * The service UUID goes in the scan response. Flags, TX power and the name
     * already use most of the 31-byte advertisement, and a 128-bit UUID needs
     * 18 more — together they would not fit, and ble_gap_adv_set_fields()
     * would simply fail.
     */
    if (s_have_adv_uuid) {
        struct ble_hs_adv_fields rsp = {0};
        rsp.uuids128             = &s_adv_uuid;
        rsp.num_uuids128         = 1;
        rsp.uuids128_is_complete = 1;

        rc = ble_gap_adv_rsp_set_fields(&rsp);
        if (rc != 0) {
            ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
            return ESP_FAIL;
        }
    }

    /* NimBLE counts advertising intervals in 0.625 ms units. */
    uint16_t itvl = (uint16_t)((s_adv_interval_ms * 1000) / 625);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = itvl,
        .itvl_max  = itvl,
    };

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);

    if (rc == BLE_HS_EALREADY) {
        /* Expected: advertising is restarted on connect so further centrals
         * can join, so the restart on disconnect often finds it already
         * running. Advertising is on, which is all the caller wants. */
        s_advertising = true;
        return ESP_OK;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        s_advertising = false;
        return ESP_FAIL;
    }

    s_advertising = true;
    ESP_LOGI(TAG, "Advertising as \"%s\" every %" PRIu32 " ms",
             name, s_adv_interval_ms);
    return ESP_OK;
}

esp_err_t bsp_ble_advertise_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    int rc = ble_gap_adv_stop();
    /* BLE_HS_EALREADY simply means advertising was not running. */
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}

/* ── GAP events ─────────────────────────────────────────────────── */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (s_conn_count < MAX_CONN) {
                s_conns[s_conn_count++] = event->connect.conn_handle;
            }
            s_connected   = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Central connected (handle %u, %d/%d)",
                     s_conn_handle, s_conn_count, MAX_CONN);
            if (s_conn_cb) s_conn_cb(s_conn_handle, true);

            /*
             * Ask the peer to pair. Windows will not do this on its own for a
             * generic GATT peripheral, so without it no bond is ever formed.
             * EALREADY simply means the link is already encrypted from a
             * previous bond, which is the desired end state anyway.
             */
            if (s_require_pairing) {
                int src = ble_gap_security_initiate(event->connect.conn_handle);
                if (src == 0) {
                    ESP_LOGI(TAG, "Requested pairing");
                } else if (src == BLE_HS_EALREADY) {
                    ESP_LOGI(TAG, "Link already encrypted from a stored bond");
                } else {
                    ESP_LOGW(TAG, "Pairing request failed: %d "
                                  "(continuing unencrypted)", src);
                }
            }

            /* Keep advertising so further centrals can still find us. */
            s_advertising = false;
            if (s_conn_count < MAX_CONN) {
                bsp_ble_advertise_start();
            } else {
                ESP_LOGI(TAG, "All %d connection slots in use — not advertising",
                         MAX_CONN);
            }
        } else {
            ESP_LOGW(TAG, "Connection failed (status %d) — advertising again",
                     event->connect.status);
            bsp_ble_advertise_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t gone = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "Central disconnected (handle %u, reason %d)",
                 gone, event->disconnect.reason);

        for (int i = 0; i < s_conn_count; i++) {
            if (s_conns[i] == gone) {
                s_conns[i] = s_conns[--s_conn_count];
                break;
            }
        }
        s_connected   = (s_conn_count > 0);
        s_conn_handle = s_connected ? s_conns[0] : BLE_HS_CONN_HANDLE_NONE;

        if (s_conn_cb) s_conn_cb(gone, false);

        /* A peripheral is useless once silent — always resume advertising. */
        bsp_ble_advertise_start();
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /*
         * Windows in particular re-pairs against a device it already has keys
         * for. Without this the pairing is rejected and the connection drops,
         * which looks like "sees the device but cannot connect". Drop the old
         * bond and let the new one proceed.
         */
        ESP_LOGW(TAG, "Peer is re-pairing — deleting the previous bond");
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        bool bonded = false;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            bonded = desc.sec_state.bonded;
        }
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Link encrypted%s", bonded ? " and bonded" : "");
        } else {
            ESP_LOGW(TAG, "Encryption failed: status %d", event->enc_change.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising ended (reason %d)", event->adv_complete.reason);
        bsp_ble_advertise_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: handle %u, notify %d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ── Host callbacks ─────────────────────────────────────────────── */
static void on_reset(int reason)
{
    /*
     * Reason 19 (BLE_HS_ETIMEOUT_HCI) here means the controller on the C6 is
     * not answering HCI at all — NimBLE retries roughly every two seconds and
     * never syncs, so nothing is ever advertised. Check the SDIO transport
     * config before suspecting the BLE code: streaming RX mode breaks HCI on
     * this board while leaving WiFi working.
     */
    ESP_LOGE(TAG, "NimBLE reset, reason %d%s", reason,
             reason == BLE_HS_ETIMEOUT_HCI ? " (controller not answering HCI)" : "");

    s_synced      = false;
    s_advertising = false;
    s_connected   = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void on_sync(void)
{
    /* The controller on the C6 is up — its address is only valid from here on. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Controller synced, address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    s_synced = true;
    bsp_ble_advertise_start();
}

static void host_task(void *arg)
{
    (void)arg;
    /* Returns only when nimble_port_stop() runs. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Lifecycle ──────────────────────────────────────────────────── */
esp_err_t bsp_ble_init(const bsp_ble_config_t *cfg)
{
    if (s_running) return ESP_OK;

    bsp_ble_config_t def = BSP_BLE_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    const char *name = cfg->device_name ? cfg->device_name : DEFAULT_DEVICE_NAME;
    s_adv_interval_ms = cfg->adv_interval_ms ? cfg->adv_interval_ms : DEFAULT_ADV_INTERVAL;
    s_conn_cb         = cfg->on_conn_change;

    /* Brings up HCI over the ESP-Hosted transport to the C6. */
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init");

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /*
     * Allow pairing and bonding. iOS happily uses an unsecured GATT server,
     * but Windows generally wants to pair before it will talk to one, and a
     * peripheral with no security manager configured simply refuses — which
     * presents as a device that scans fine but never connects.
     *
     * No input and no output means Just Works pairing: no PIN to enter, which
     * is right for a device whose only UI is a touchscreen nobody is looking
     * at during pairing. It gives no MITM protection; add sm_mitm and a
     * passkey if this ever carries anything sensitive.
     */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Application services must be added here: once the host task is running,
     * the GATT database is fixed. */
    if (cfg->register_services) {
        cfg->register_services();
    }

    s_require_pairing = cfg->require_pairing;

    if (cfg->adv_uuid128) {
        /* NimBLE stores 128-bit UUIDs least-significant byte first, while the
         * caller passes them in written order — so reverse. */
        s_adv_uuid.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) {
            s_adv_uuid.value[i] = cfg->adv_uuid128[15 - i];
        }
        s_have_adv_uuid = true;
    }

    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_running = true;
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "NimBLE host started (controller on ESP32-C6)");
    return ESP_OK;
}

esp_err_t bsp_ble_deinit(void)
{
    if (!s_running) return ESP_OK;

    bsp_ble_advertise_stop();

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_deinit();

    s_running     = false;
    s_synced      = false;
    s_advertising = false;
    s_connected   = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "NimBLE host stopped");
    return ESP_OK;
}

/* ── Status ─────────────────────────────────────────────────────── */
bool bsp_ble_is_connected(void)
{
    return s_connected;
}

bool bsp_ble_is_advertising(void)
{
    return s_advertising && !s_connected;
}

bool bsp_ble_is_ready(void)
{
    return s_synced;
}

uint16_t bsp_ble_conn_handle(void)
{
    return s_conn_handle;
}

int bsp_ble_conn_count(void)
{
    return s_conn_count;
}

uint16_t bsp_ble_conn_handle_at(int index)
{
    if (index < 0 || index >= s_conn_count) return BLE_HS_CONN_HANDLE_NONE;
    return s_conns[index];
}

esp_err_t bsp_ble_get_address(char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(out && out_len >= 18, ESP_ERR_INVALID_ARG, TAG, "buffer too small");
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not running");

    uint8_t addr[6] = {0};
    int rc = ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    if (rc != 0) return ESP_ERR_INVALID_STATE;

    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return ESP_OK;
}
