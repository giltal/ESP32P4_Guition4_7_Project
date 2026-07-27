/*
 * Guition JC4880P443C-I-W (ESP32-P4) â€” application entry point.
 *
 * Single application image, updated in place over the air between the
 * ota_0 and ota_1 slots. See partitions.csv.
 */

#include <inttypes.h>

#include "bsp.h"
#include "bsp_wifi.h"
#include "bsp_wifi_prov.h"
#include "bsp_ble.h"
#include "bsp_ota.h"
#include "bsp_ota_web.h"
#include "bsp_lvgl.h"
#include "bsp_remote.h"
#include "bsp_copro.h"
#include "ble_control.h"
#include "ble_uart.h"
#include "ble_console.h"
#include "ui.h"
#include "board_pins.h"
#include "st7701_lcd.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

/*
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set, so an image installed over the
 * air boots exactly once on trial. If it does not confirm itself the bootloader
 * reverts to the previous slot on the next reset. Self-checks that decide
 * whether this build is healthy belong here, before it is marked valid.
 *
 * Confirming is also a precondition for starting the *next* update, so a build
 * that never reaches this point can never be replaced over the air either.
 */
static void confirm_or_rollback(void)
{
    if (!bsp_ota_is_pending_verify()) return;

    ESP_LOGI(TAG, "Image is on trial â€” running health checks");

    /* Minimal check for now: the panel came up. Extend as the app grows. */
    bool healthy = (st7701_lcd_width() == BSP_LCD_H_RES);

    if (healthy) {
        bsp_ota_mark_valid();
    } else {
        ESP_LOGE(TAG, "Health check failed â€” rolling back");
        bsp_ota_rollback_and_reboot();
    }
}

/*
 * Why the last boot happened, and how many boots there have been.
 *
 * Without this a spontaneous restart leaves nothing behind: core dumps are
 * disabled and there is no coredump partition, so the reset reason recorded by
 * the ROM is the only clue that survives.
 */
static void log_reset_reason(void)
{
    static const char *const NAMES[] = {
        [ESP_RST_UNKNOWN]  = "unknown",   [ESP_RST_POWERON]  = "power-on",
        [ESP_RST_EXT]      = "external",  [ESP_RST_SW]       = "software",
        [ESP_RST_PANIC]    = "PANIC",     [ESP_RST_INT_WDT]  = "INTERRUPT WATCHDOG",
        [ESP_RST_TASK_WDT] = "TASK WATCHDOG", [ESP_RST_WDT]  = "OTHER WATCHDOG",
        [ESP_RST_DEEPSLEEP] = "deep sleep", [ESP_RST_BROWNOUT] = "BROWNOUT",
        [ESP_RST_SDIO]     = "sdio",      [ESP_RST_USB]      = "usb",
        [ESP_RST_JTAG]     = "jtag",
    };

    esp_reset_reason_t reason = esp_reset_reason();
    const char *name = (reason < sizeof(NAMES) / sizeof(NAMES[0]) && NAMES[reason])
                           ? NAMES[reason] : "?";

    uint32_t     boots = 0;
    nvs_handle_t nvs;
    if (nvs_open("app_diag", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boots", &boots);
        boots++;
        nvs_set_u32(nvs, "boots", boots);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Boot #%" PRIu32 ", last reset: %s (%d)", boots, name, (int)reason);
}

/*
 * Heartbeat. Silence on the console is ambiguous â€” an idle device and a wedged
 * one look identical â€” so print uptime and free memory periodically. If the log
 * stops, the last line says how long it survived and whether memory was
 * draining beforehand.
 */
static void heartbeat_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        /* WiFi state and recovery count are included so a lost link is
         * visible in the log even when nothing else reports it. */
        esp_ip4_addr_t ip = {0};
        bsp_wifi_get_ip(&ip);

        ESP_LOGI(TAG,
                 "alive %" PRIu32 " s | wifi %d " IPSTR " recov %" PRIu32
                 " | heap int %u (min %u) | psram %u",
                 (uint32_t)(esp_timer_get_time() / 1000000),
                 (int)bsp_wifi_state(), IP2STR(&ip), bsp_wifi_link_recoveries(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

static void log_boot_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *desc    = esp_app_get_description();

    ESP_LOGI(TAG, "Firmware %s (%s %s)", desc->version, desc->date, desc->time);
    ESP_LOGI(TAG, "Running from '%s' at offset 0x%" PRIx32 ", size %" PRIu32 " KB",
             running->label, running->address, running->size / 1024);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        ESP_LOGI(TAG, "Next OTA target: '%s' at 0x%" PRIx32, next->label, next->address);
    }
}

#define SD_UPDATE_IMAGE "/sd/firmware.bin"

static void on_ota_progress(size_t written, size_t total, void *user)
{
    (void)user;
    static int last_pct = -1;

    int pct = total ? (int)((uint64_t)written * 100 / total) : -1;
    if (pct != last_pct && pct % 10 == 0) {
        ESP_LOGI(TAG, "Update %d%% (%u/%u bytes)", pct, (unsigned)written, (unsigned)total);
        last_pct = pct;
    }
}

/*
 * Offline update path: drop a firmware.bin on the SD card and power-cycle.
 * An image matching the running version is refused by bsp_ota, so leaving the
 * file in place does not cause an update loop.
 */
static void check_sd_update(void)
{
    if (!bsp_sdcard_is_mounted()) return;

    FILE *f = fopen(SD_UPDATE_IMAGE, "rb");
    if (f == NULL) return;
    fclose(f);

    ESP_LOGI(TAG, "Found %s â€” attempting update", SD_UPDATE_IMAGE);

    bsp_ota_config_t cfg = BSP_OTA_CONFIG_DEFAULT();
    cfg.on_progress = on_ota_progress;

    /* Reboots into the new image on success and does not return. */
    esp_err_t err = bsp_ota_update_from_file(SD_UPDATE_IMAGE, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD update not applied: %s", esp_err_to_name(err));
    }
}

/*
 * Network update path. The URL lives in NVS rather than being compiled in, so
 * the same image can be pointed at a staging or production server after the
 * fact:  nvs "app_ota" / key "url".
 */
static void check_network_update(void)
{
    nvs_handle_t nvs;
    if (nvs_open("app_ota", NVS_READONLY, &nvs) != ESP_OK) return;

    char   url[256] = {0};
    size_t len      = sizeof(url);
    esp_err_t err   = nvs_get_str(nvs, "url", url, &len);
    nvs_close(nvs);

    if (err != ESP_OK || url[0] == '\0') return;

    ESP_LOGI(TAG, "Checking for updates at %s", url);

    bsp_ota_config_t cfg = BSP_OTA_CONFIG_DEFAULT();
    cfg.on_progress = on_ota_progress;

    err = bsp_ota_update_from_url(url, &cfg);
    if (err == ESP_ERR_INVALID_VERSION) {
        ESP_LOGI(TAG, "Already running the published version");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Update check failed: %s", esp_err_to_name(err));
    }
}

/* Runs in the one window where NimBLE accepts new services: after the standard
 * GAP/GATT services exist, before the host task starts. */
static void register_ble_services(void)
{
    ble_control_register();
    ble_uart_register();
    ble_console_start();
}

static void on_ble_conn_change(uint16_t conn_handle, bool connected)
{
    ESP_LOGI(TAG, "BLE central %s (handle %u)",
             connected ? "connected" : "disconnected", conn_handle);
}

/*
 * WiFi and BLE share one SDIO link to the C6, so the WiFi stack is brought up
 * first: it opens the ESP-Hosted transport that NimBLE's HCI then rides on.
 */
static void start_radios(void)
{
    esp_err_t err = bsp_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed (%s) â€” no radio, skipping BLE too",
                 esp_err_to_name(err));
        return;
    }

    /*
     * The hosted transport is up now, so this is the point to bring the C6's
     * firmware in line with the host component. Does not return if it updates:
     * both sides restart together.
     */
    bsp_copro_update_if_needed();

    /*
     * BLE before WiFi setup: it rides the same C6 link but does not depend on
     * being on a network, and provisioning can take arbitrarily long.
     */
    bsp_ble_config_t ble = BSP_BLE_CONFIG_DEFAULT();
    ble.device_name       = "gution-p4";
    ble.on_conn_change    = on_ble_conn_change;
    ble.register_services = register_ble_services;
    /*
     * Leave pairing unrequested. Asking Windows to pair on connect was tried
     * to satisfy applications that only talk to bonded devices: Windows
     * rejects the request outright, drops the link, and retries in a tight
     * connect/disconnect loop that also breaks clients which were working.
     * Windows will not bond with a generic GATT peripheral by any means the
     * peripheral controls; clients must talk to it unbonded.
     */
    ble.require_pairing   = false;
    /*
     * Only one 128-bit UUID fits in the scan response (18 of 31 bytes each),
     * and the UART one earns the slot: terminal apps use it to decide whether
     * to offer a console. The control service is still found on connect.
     */
    ble.adv_uuid128       = ble_uart_service_uuid;
    if (bsp_ble_init(&ble) != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed");
    }

    /*
     * With no stored credentials the device can reach nothing, so put up the
     * setup access point. It is only a fallback â€” the UI's WiFi tab does the
     * same job, and stops the AP itself before connecting. So wait on the
     * credentials appearing by *either* route rather than on the portal's own
     * completion, which the on-screen path would never signal.
     */
    if (bsp_wifi_prov_is_needed()) {
        ESP_LOGW(TAG, "No stored credentials â€” starting WiFi setup");

        bsp_wifi_prov_config_t prov = BSP_WIFI_PROV_CONFIG_DEFAULT();
        if (bsp_wifi_prov_start(&prov) == ESP_OK) {
            while (!bsp_wifi_has_credentials()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            bsp_wifi_prov_stop();   /* no-op if the UI already stopped it */
        }
    }

    /* The UI path leaves us already connected; do not re-issue a connect. */
    if (bsp_wifi_is_connected() || bsp_wifi_connect_saved() == ESP_OK) {
        if (bsp_wifi_is_connected() || bsp_wifi_wait_connected(20000) == ESP_OK) {
            esp_ip4_addr_t ip;
            int rssi;
            bsp_wifi_get_ip(&ip);
            ESP_LOGI(TAG, "WiFi up: " IPSTR "%s", IP2STR(&ip),
                     bsp_wifi_get_rssi(&rssi) == ESP_OK ? "" : " (no RSSI)");
            check_network_update();

            /* Reachable at http://<ip>/ for as long as the device is up.
             * Left open here; set username/password before deploying. */
            bsp_ota_web_config_t web = BSP_OTA_WEB_CONFIG_DEFAULT();
            if (bsp_ota_web_start(&web) == ESP_OK) {
                ESP_LOGI(TAG, "Firmware upload page at http://" IPSTR "/", IP2STR(&ip));
            }
        } else {
            ESP_LOGW(TAG, "WiFi did not connect within 20 s â€” retrying in background");
        }
    } else {
        ESP_LOGW(TAG, "No stored WiFi credentials");
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_reset_reason();
    log_boot_info();
    xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 1, NULL);

    bsp_config_t cfg = BSP_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(bsp_init(&cfg));

    /* Only confirm once the board has actually come up. */
    confirm_or_rollback();

    ESP_LOGI(TAG, "Ready â€” %ux%u, SD %s",
             st7701_lcd_width(), st7701_lcd_height(),
             bsp_sdcard_is_mounted() ? "mounted" : "absent");

    /* Offline update first â€” it needs no network and reboots if it applies. */
    check_sd_update();

    /* Shared control state must exist before the UI subscribes to it. */
    ESP_ERROR_CHECK(bsp_remote_init());

    /* Bring the UI up before the radios: connecting can take 20 s, and the
     * screen should be showing something the whole time. */
    ESP_ERROR_CHECK(bsp_lvgl_init());
    ESP_ERROR_CHECK(ui_init());

    start_radios();

    /*
     * The UI owns the screen now, and LVGL's input device polls the GT911
     * itself. The old bring-up loop that logged touch coordinates has been
     * removed: it read the same I2C device on a second task, competing with
     * LVGL for the controller, and flooded the log during normal use.
     *
     * app_main may simply return â€” the LVGL, radio and server tasks keep
     * running, and the main task's stack is reclaimed.
     */
    ESP_LOGI(TAG, "Startup complete");
}
