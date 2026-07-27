/*
 * Nordic UART Service — see ble_uart.h.
 */

#include "ble_uart.h"
#include "bsp_ble.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"

static const char *TAG = "ble_uart";

/*
 * 6e40000X-b5a3-f393-e0a9-e50e24dcca9e, reversed because NimBLE stores
 * 128-bit UUIDs least-significant byte first.
 */
#define NUS_TAIL 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                 0x93, 0xf3, 0xa3, 0xb5

#define NUS_UUID(id) BLE_UUID128_DECLARE(NUS_TAIL, (id), 0x00, 0x40, 0x6e)

#define ID_SVC 0x01
#define ID_RX  0x02
#define ID_TX  0x03

/* Written order, for advertising. */
const uint8_t ble_uart_service_uuid[16] = {
    0x6e, 0x40, 0x00, 0x01, 0xb5, 0xa3, 0xf3, 0x93,
    0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e,
};

static uint16_t         s_h_tx;
static ble_uart_rx_cb_t s_rx_cb;
static void            *s_rx_user;

/* ── Access ─────────────────────────────────────────────────────── */
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;

    /* One MTU's worth at most, so a stack buffer is bounded and safe. */
    uint8_t  buf[BLE_ATT_MTU_MAX];
    uint16_t got = 0;
    if (len > sizeof(buf)) len = sizeof(buf);

    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_rx_cb) s_rx_cb(buf, got, s_rx_user);
    return 0;
}

/* ── Service definition ─────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = NUS_UUID(ID_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Central writes here. Write-without-response is what terminal
                 * apps use for throughput; both are accepted. */
                .uuid      = NUS_UUID(ID_RX),
                .access_cb = chr_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* Device notifies here. Not readable: NUS is a stream, there
                 * is no "current value" to read back. */
                .uuid       = NUS_UUID(ID_TX),
                .access_cb  = chr_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_h_tx,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ── Send ───────────────────────────────────────────────────────── */
bool ble_uart_is_connected(void)
{
    return bsp_ble_is_connected();
}

/** Send to one peer, split to its MTU. */
static esp_err_t send_to(uint16_t conn, const uint8_t *data, size_t len)
{
    /* Three bytes of the MTU go to the ATT notification header. Each peer
     * negotiates its own, so this cannot be hoisted out of the loop. */
    uint16_t mtu   = ble_att_mtu(conn);
    size_t   chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    size_t left = len;
    while (left > 0) {
        size_t n = left < chunk ? left : chunk;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
        if (om == NULL) return ESP_ERR_NO_MEM;

        int rc = ble_gatts_notify_custom(conn, s_h_tx, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify to %u failed: %d", conn, rc);
            return ESP_FAIL;
        }

        data += n;
        left -= n;
    }
    return ESP_OK;
}

esp_err_t ble_uart_send(const void *data, size_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    int peers = bsp_ble_conn_count();
    if (peers == 0) return ESP_ERR_INVALID_STATE;

    /* Broadcast to every connected central, so a terminal on a PC and one on
     * a phone see the same session. */
    esp_err_t last = ESP_OK;
    for (int i = 0; i < peers; i++) {
        esp_err_t err = send_to(bsp_ble_conn_handle_at(i), data, len);
        if (err != ESP_OK) last = err;
    }
    return last;
}

esp_err_t ble_uart_printf(const char *fmt, ...)
{
    char    buf[256];
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n <= 0) return ESP_ERR_INVALID_ARG;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   /* truncated */

    return ble_uart_send(buf, (size_t)n);
}

/* ── Registration ───────────────────────────────────────────────── */
void ble_uart_set_rx_handler(ble_uart_rx_cb_t cb, void *user)
{
    s_rx_cb   = cb;
    s_rx_user = user;
}

void ble_uart_register(void)
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

    ESP_LOGI(TAG, "UART service registered (NUS)");
}
