/*
 * BLE GATT service exposing the shared control state — see ble_control.h.
 */

#include "ble_control.h"

#include <string.h>

#include "bsp_ble.h"
#include "bsp_remote.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_ctl";

/*
 * 4f3a00XX-9d2b-4c7e-a1f5-8b6d3e0c7a21, XX distinguishing the members.
 * BLE_UUID128_DECLARE wants the bytes least-significant first, i.e. reversed
 * from how the UUID is written — hence each row reading backwards.
 */
#define UUID_TAIL 0x21, 0x7a, 0x0c, 0x3e, 0x6d, 0x8b, 0xf5, 0xa1, \
                  0x7e, 0x4c, 0x2b, 0x9d

#define UUID_OF(id) BLE_UUID128_DECLARE(UUID_TAIL, (id), 0x00, 0x3a, 0x4f)

/* Written order, for advertising. */
const uint8_t ble_control_service_uuid[16] = {
    0x4f, 0x3a, 0x00, 0x01, 0x9d, 0x2b, 0x4c, 0x7e,
    0xa1, 0xf5, 0x8b, 0x6d, 0x3e, 0x0c, 0x7a, 0x21,
};

static uint16_t s_h_mode, s_h_level, s_h_led, s_h_label;

/* Which characteristic a request refers to, decided by the UUID's 4th byte. */
enum { ID_SVC = 0x01, ID_MODE = 0x02, ID_LEVEL = 0x03, ID_LED = 0x04, ID_LABEL = 0x05 };

/* ── Access ─────────────────────────────────────────────────────── */
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle;

    const uint8_t id = (uint8_t)(uintptr_t)arg;

    bsp_remote_state_t s;
    bsp_remote_get(&s);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v;
        switch (id) {
        case ID_MODE:  v = (uint8_t)s.mode;  break;
        case ID_LEVEL: v = (uint8_t)s.level; break;
        case ID_LED:   v = s.led ? 1 : 0;    break;
        case ID_LABEL:
            return os_mbuf_append(ctxt->om, s.label, strlen(s.label)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, &v, sizeof(v)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

        if (id == ID_LABEL) {
            if (len > BSP_REMOTE_LABEL_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            char buf[BSP_REMOTE_LABEL_MAX + 1] = {0};
            uint16_t got = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &got) != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            buf[got] = '\0';
            strlcpy(s.label, buf, sizeof(s.label));
        } else {
            if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            uint8_t v = 0;
            uint16_t got = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, &v, sizeof(v), &got) != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }

            switch (id) {
            case ID_MODE:
                if (v >= BSP_REMOTE_MODE_COUNT) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                s.mode = (bsp_remote_mode_t)v;
                break;
            case ID_LEVEL:
                if (v > 100) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                s.level = v;
                break;
            case ID_LED:
                s.led = (v != 0);
                break;
            default:
                return BLE_ATT_ERR_UNLIKELY;
            }
        }

        /* Publishes to the display and the web page as well. */
        bsp_remote_set(&s);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── Service definition ─────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = UUID_OF(ID_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = UUID_OF(ID_MODE),
                .access_cb  = chr_access,
                .arg        = (void *)(uintptr_t)ID_MODE,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_h_mode,
            },
            {
                .uuid       = UUID_OF(ID_LEVEL),
                .access_cb  = chr_access,
                .arg        = (void *)(uintptr_t)ID_LEVEL,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_h_level,
            },
            {
                .uuid       = UUID_OF(ID_LED),
                .access_cb  = chr_access,
                .arg        = (void *)(uintptr_t)ID_LED,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_h_led,
            },
            {
                .uuid       = UUID_OF(ID_LABEL),
                .access_cb  = chr_access,
                .arg        = (void *)(uintptr_t)ID_LABEL,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_h_label,
            },
            { 0 },   /* terminator */
        },
    },
    { 0 },
};

/* ── Notifications ──────────────────────────────────────────────── */
/*
 * Called by bsp_remote after any change, whoever made it — a BLE write, the
 * web page, or the touchscreen. Notifying unconditionally is harmless: NimBLE
 * drops notifications for characteristics the central has not subscribed to.
 */
static void notify_one(uint16_t conn, uint16_t handle, const void *data, size_t len)
{
    /* Each notification consumes its mbuf, so build a fresh one per peer. */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) ble_gatts_notify_custom(conn, handle, om);
}

/*
 * Only the fields that actually changed are notified.
 *
 * Notifying all four on every change quadrupled the traffic, and a burst of
 * writes — a client scripting the device, or a slider being dragged — could
 * then outrun NimBLE's mbuf pool and take the connection down with it. Most
 * changes touch one field.
 */
static void on_remote_changed(const bsp_remote_state_t *state, void *user)
{
    (void)user;

    static bsp_remote_state_t s_last;
    static bool               s_have_last;

    bool send_mode  = !s_have_last || state->mode  != s_last.mode;
    bool send_level = !s_have_last || state->level != s_last.level;
    bool send_led   = !s_have_last || state->led   != s_last.led;
    bool send_label = !s_have_last || strcmp(state->label, s_last.label) != 0;

    s_last      = *state;
    s_have_last = true;

    uint8_t mode  = (uint8_t)state->mode;
    uint8_t level = (uint8_t)state->level;
    uint8_t led   = state->led ? 1 : 0;

    /* Every connected central, not just the first — a phone and a PC can both
     * be subscribed. NimBLE discards notifications for peers that have not. */
    for (int i = 0; i < bsp_ble_conn_count(); i++) {
        uint16_t conn = bsp_ble_conn_handle_at(i);

        if (send_mode)  notify_one(conn, s_h_mode,  &mode,  sizeof(mode));
        if (send_level) notify_one(conn, s_h_level, &level, sizeof(level));
        if (send_led)   notify_one(conn, s_h_led,   &led,   sizeof(led));
        if (send_label) notify_one(conn, s_h_label, state->label,
                                   strlen(state->label));
    }
}

/* ── Registration ───────────────────────────────────────────────── */
void ble_control_register(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return;
    }

    bsp_remote_subscribe(on_remote_changed, NULL);
    ESP_LOGI(TAG, "Control service registered (mode, level, led, message)");
}
