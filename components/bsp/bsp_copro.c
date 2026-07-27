/*
 * ESP32-C6 co-processor firmware management — see bsp_copro.h.
 */

#include "bsp_copro.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_ota.h"

static const char *TAG = "bsp_copro";

/*
 * Version of the embedded slave image — must match whatever c6fw_old/ built,
 * or the automatic check would push it and then immediately see a "mismatch"
 * again on the next boot.
 *
 * Deliberately NOT the newest. esp-hosted-mcu issue #115 reports that NimBLE
 * HCI stops responding with slave firmware newer than 2.5.1, which is exactly
 * what this board showed on 2.12.11: WiFi fine, every HCI command timing out,
 * BLE never advertising. That issue is still open upstream, so the slave is
 * pinned to a version predating the regression. The host stays on 2.12.x —
 * the combination this board originally shipped and worked with.
 */
#define TARGET_MAJOR 2
#define TARGET_MINOR 0
#define TARGET_PATCH 17

/* rpc_wrap.c splits transfers at 1400 bytes; matching it avoids re-chunking. */
#define CHUNK_SIZE 1400

/*
 * An automatic update that keeps failing must not retry on every boot: each
 * attempt rewrites the C6's inactive slot, and a boot loop around that has no
 * recovery path on this module. Forced updates ignore this.
 */
#define MAX_ATTEMPTS  2
#define NVS_NAMESPACE "app_diag"
#define NVS_KEY_TRIES "copro_try"

extern const uint8_t slave_fw_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t slave_fw_end[]   asm("_binary_network_adapter_bin_end");

static uint32_t attempts_get(void)
{
    uint32_t     n = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_KEY_TRIES, &n);
        nvs_close(nvs);
    }
    return n;
}

static void attempts_set(uint32_t n)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_KEY_TRIES, n);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/** Perform the transfer. Does not return on success. */
static esp_err_t push_image(bool activate_supported)
{
    const size_t total = (size_t)(slave_fw_end - slave_fw_start);

    ESP_LOGW(TAG, "Writing %u bytes to the co-processor — do not power off",
             (unsigned)total);

    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t sent     = 0;
    int    last_bucket = -1;
    while (sent < total) {
        size_t n = total - sent;
        if (n > CHUNK_SIZE) n = CHUNK_SIZE;

        /* The API is not const-correct; the data is not modified. */
        err = esp_hosted_slave_ota_write((uint8_t *)(slave_fw_start + sent), n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %u/%u: %s",
                     (unsigned)sent, (unsigned)total, esp_err_to_name(err));
            return err;
        }
        sent += n;

        int pct = (int)((uint64_t)sent * 100 / total);
        if (pct / 10 != last_bucket) {
            last_bucket = pct / 10;
            ESP_LOGI(TAG, "  %d%% (%u/%u)", pct, (unsigned)sent, (unsigned)total);
        }
    }

    /* Validates the image on the C6 before it can be booted. */
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Explicit activation exists only from slave 2.6.0 onward; older slaves
     * switch the boot partition inside ota_end(), where calling activate would
     * simply fail.
     */
    if (activate_supported) {
        err = esp_hosted_slave_ota_activate();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_activate failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "New co-processor firmware activated");
    } else {
        ESP_LOGI(TAG, "Slave predates the activate API — activated by ota_end()");
    }

    attempts_set(0);

    /* Both sides must restart, or the transport is left talking to firmware
     * that changed underneath it. */
    ESP_LOGW(TAG, "Co-processor updated — restarting host to resynchronise");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;   /* not reached */
}

static bool read_version(esp_hosted_coprocessor_fwver_t *ver)
{
    if (esp_hosted_get_coprocessor_fwversion(ver) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read the co-processor version");
        return false;
    }
    ESP_LOGI(TAG, "Co-processor firmware %" PRId32 ".%" PRId32 ".%" PRId32
                  " (this build carries %d.%d.%d)",
             ver->major1, ver->minor1, ver->patch1,
             TARGET_MAJOR, TARGET_MINOR, TARGET_PATCH);
    return true;
}

static bool supports_activate(const esp_hosted_coprocessor_fwver_t *ver)
{
    return (ver->major1 > 2) || (ver->major1 == 2 && ver->minor1 > 5);
}

esp_err_t bsp_copro_update_if_needed(void)
{
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (!read_version(&ver)) return ESP_ERR_INVALID_STATE;

    if (ver.major1 == TARGET_MAJOR && ver.minor1 == TARGET_MINOR &&
        ver.patch1 == TARGET_PATCH) {
        ESP_LOGI(TAG, "Co-processor is up to date");
        attempts_set(0);
        return ESP_OK;
    }

    uint32_t tries = attempts_get();
    if (tries >= MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "Update already failed %" PRIu32 " times — not retrying. "
                      "Force it explicitly, or clear NVS %s/%s.",
                 tries, NVS_NAMESPACE, NVS_KEY_TRIES);
        return ESP_FAIL;
    }
    attempts_set(tries + 1);

    ESP_LOGW(TAG, "Version differs — updating (attempt %" PRIu32 "/%d)",
             tries + 1, MAX_ATTEMPTS);
    return push_image(supports_activate(&ver));
}

esp_err_t bsp_copro_update_force(void)
{
    esp_hosted_coprocessor_fwver_t ver = {0};
    bool have_version = read_version(&ver);

    ESP_LOGW(TAG, "Forced co-processor update requested");

    /* Without a readable version, assume the older behaviour and let ota_end()
     * do the activation — calling activate on a slave that lacks it fails,
     * whereas skipping it on a newer slave is handled by ota_end() anyway. */
    return push_image(have_version && supports_activate(&ver));
}
