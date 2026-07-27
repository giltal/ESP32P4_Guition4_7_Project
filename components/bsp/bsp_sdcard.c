/*
 * SD card — SDMMC slot 0, 4-bit, powered from the P4 on-chip LDO.
 *
 * Some cards need a power cycle before they enumerate, so a failed mount is
 * retried with the LDO torn down and recreated in between.
 */

#include "bsp.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_sd";

#define SD_MOUNT_ATTEMPTS 3

static bool                  s_mounted;
static sdmmc_card_t         *s_card;
static sd_pwr_ctrl_handle_t  s_pwr_ctrl;

bool bsp_sdcard_is_mounted(void)
{
    return s_mounted;
}

esp_err_t bsp_sdcard_mount(void)
{
    if (s_mounted) return ESP_OK;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = BSP_SD_LDO_CHAN };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO create failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 16 * 1024,
    };

    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        sdmmc_host_t host   = SDMMC_HOST_DEFAULT();
        host.slot           = SDMMC_HOST_SLOT_0;
        host.max_freq_khz   = SDMMC_FREQ_HIGHSPEED;
        host.pwr_ctrl_handle = s_pwr_ctrl;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk   = BSP_SD_CLK_GPIO;
        slot.cmd   = BSP_SD_CMD_GPIO;
        slot.d0    = BSP_SD_D0_GPIO;
        slot.d1    = BSP_SD_D1_GPIO;
        slot.d2    = BSP_SD_D2_GPIO;
        slot.d3    = BSP_SD_D3_GPIO;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot,
                                      &mount_cfg, &s_card);
        if (err == ESP_OK) {
            s_mounted = true;
            ESP_LOGI(TAG, "Mounted at %s (attempt %d): %s, %lluMB",
                     BSP_SD_MOUNT_POINT, attempt, s_card->cid.name,
                     ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Mount attempt %d/%d failed: %s",
                 attempt, SD_MOUNT_ATTEMPTS, esp_err_to_name(err));

        if (attempt < SD_MOUNT_ATTEMPTS) {
            sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
            s_pwr_ctrl = NULL;
            vTaskDelay(pdMS_TO_TICKS(500));

            esp_err_t ldo_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
            if (ldo_err != ESP_OK) {
                ESP_LOGE(TAG, "LDO recreate failed: %s", esp_err_to_name(ldo_err));
                return ldo_err;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    /* Out of attempts — release the LDO so a later retry starts clean. */
    if (s_pwr_ctrl) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }
    ESP_LOGE(TAG, "SD card not mounted after %d attempts", SD_MOUNT_ATTEMPTS);
    return err;
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (!s_mounted) return ESP_OK;

    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_card    = NULL;
    s_mounted = false;

    if (s_pwr_ctrl) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }
    ESP_LOGI(TAG, "Unmounted %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}
