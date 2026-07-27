/*
 * Board Support Package — Guition JC4880P443C-I-W (ESP32-P4)
 */

#include "bsp.h"
#include "board_pins.h"

#include "st7701_lcd.h"
#include "gt911_touch.h"
#include "ppa_engine.h"
#include "audio.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_DUTY_RES     LEDC_TIMER_13_BIT
#define BL_DUTY_MAX     ((1u << 13) - 1u)
#define BL_FREQ_HZ      5000

static bool                     s_initialized;
static bool                     s_backlight_ready;
static uint8_t                  s_brightness;
static i2c_master_bus_handle_t  s_i2c_bus;

/* ── I2C ────────────────────────────────────────────────────────── */
/*
 * Must live on BSP_I2C_PORT: gt911_touch_init() resolves the bus by port
 * number via i2c_master_get_bus_handle() instead of taking a handle.
 */
static esp_err_t bsp_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port          = BSP_I2C_PORT,
        .sda_io_num        = BSP_I2C_SDA_GPIO,
        .scl_io_num        = BSP_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags             = { .enable_internal_pullup = 1 },
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C port %d up (SDA=%d SCL=%d)",
             BSP_I2C_PORT, BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return s_i2c_bus;
}

/* ── Backlight ──────────────────────────────────────────────────── */
static esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t channel = {
        .gpio_num   = BSP_LCD_BK_LIGHT_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");

    s_backlight_ready = true;
    ESP_LOGI(TAG, "Backlight on GPIO %d", BSP_LCD_BK_LIGHT_GPIO);
    return ESP_OK;
}

esp_err_t bsp_backlight_set(uint8_t percent)
{
    if (!s_backlight_ready) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;

    uint32_t duty = (BL_DUTY_MAX * percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL), TAG, "update duty");

    s_brightness = percent;
    return ESP_OK;
}

uint8_t bsp_backlight_get(void)
{
    return s_brightness;
}

/* ── Audio ──────────────────────────────────────────────────────── */
static esp_err_t bsp_audio_init(void)
{
    audio_config_t cfg = AUDIO_CONFIG_DEFAULT();
    cfg.mclk_io    = BSP_I2S_MCLK_GPIO;
    cfg.bclk_io    = BSP_I2S_BCLK_GPIO;
    cfg.ws_io      = BSP_I2S_WS_GPIO;
    cfg.dout_io    = BSP_I2S_DOUT_GPIO;
    cfg.din_io     = BSP_I2S_DIN_GPIO;
    cfg.pa_ctrl_io = BSP_AUDIO_PA_GPIO;
    cfg.i2c_handle = s_i2c_bus;
    return audio_init(&cfg);
}

/* ── Init ───────────────────────────────────────────────────────── */
esp_err_t bsp_init(const bsp_config_t *cfg)
{
    if (s_initialized) return ESP_OK;

    bsp_config_t def = BSP_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    ESP_LOGI(TAG, "Board init — Guition JC4880P443C-I-W");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(),    TAG, "i2c");
    ESP_RETURN_ON_ERROR(ppa_engine_init(), TAG, "ppa");

    if (cfg->display) {
        /* Backlight starts at 0 so the panel's first frame is not a white flash. */
        ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "backlight");
        ESP_RETURN_ON_ERROR(st7701_lcd_init(),    TAG, "lcd");
        st7701_lcd_fill_screen(0x0000);
        vTaskDelay(pdMS_TO_TICKS(50));
        bsp_backlight_set(cfg->brightness);
        ESP_LOGI(TAG, "Panel up: %ux%u", st7701_lcd_width(), st7701_lcd_height());

        if (cfg->touch) {
            ESP_RETURN_ON_ERROR(
                gt911_touch_init(BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO,
                                 BSP_TOUCH_RST_GPIO, BSP_TOUCH_INT_GPIO),
                TAG, "touch");
        }
    } else if (cfg->touch) {
        ESP_LOGW(TAG, "touch requested without display — skipped");
    }

    /* Non-fatal: the board is still usable without audio or a card. */
    if (cfg->audio) {
        esp_err_t err = bsp_audio_init();
        if (err != ESP_OK) ESP_LOGW(TAG, "audio init failed (%s) — continuing", esp_err_to_name(err));
    }

    if (cfg->sdcard) {
        esp_err_t err = bsp_sdcard_mount();
        if (err != ESP_OK) ESP_LOGW(TAG, "SD mount failed (%s) — continuing", esp_err_to_name(err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Board init complete");
    return ESP_OK;
}
