/*
 * LVGL binding — ST7701S MIPI-DSI panel + GT911 touch.
 */

#include "bsp_lvgl.h"
#include "board_pins.h"

#include "st7701_lcd.h"
#include "gt911_touch.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"

static const char *TAG = "bsp_lvgl";

/*
 * Partial rendering into LVGL's own PSRAM buffers, rather than
 * esp_lvgl_port's tear-free mode. Tear-free binds LVGL directly to the DPI
 * panel's internal framebuffers and needs the panel created with at least two
 * of them; st7701_lcd_init() creates one. To switch: raise `num_fbs` to 2 in
 * st7701_lcd.c and set dsi_cfg.flags.avoid_tearing below. That costs another
 * 768 KB of PSRAM and is worth doing if scrolling shows tearing.
 *
 * 100 lines per buffer, double buffered: 480 x 100 x 2 B = 96 KB each.
 */
#define DRAW_BUFFER_LINES 100

static lv_display_t *s_disp;
static lv_indev_t   *s_touch;

/* ── Touch ──────────────────────────────────────────────────────── */
/*
 * lvgl_port_add_touch() wants an esp_lcd_touch_handle_t, which gt911_touch
 * keeps private. Reading through its public accessor avoids reaching into the
 * driver, at the cost of single-touch only — which is all this UI needs.
 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t x = 0, y = 0;
    if (gt911_touch_get_xy(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ── Init ───────────────────────────────────────────────────────── */
esp_err_t bsp_lvgl_init(void)
{
    if (s_disp) return ESP_OK;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    bsp_lcd_handles_t lcd = {0};
    st7701_lcd_get_handles(&lcd);
    ESP_RETURN_ON_FALSE(lcd.panel, ESP_ERR_INVALID_STATE, TAG,
                        "panel not initialised — call bsp_init() first");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = lcd.io,
        .panel_handle   = lcd.panel,
        .control_handle = lcd.control,
        .buffer_size    = BSP_LCD_H_RES * DRAW_BUFFER_LINES,
        .double_buffer  = true,
        .hres           = BSP_LCD_H_RES,
        .vres           = BSP_LCD_V_RES,
        .monochrome     = false,
        .color_format   = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,   /* 2 x 96 KB is too much for internal SRAM */
            .sw_rotate   = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = { .avoid_tearing = false },
    };

    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add display");

    /* Widget tree access must be serialised against the LVGL task. */
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "could not take the LVGL lock");
        return ESP_FAIL;
    }
    s_touch = lv_indev_create();
    lv_indev_set_type(s_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch, touch_read_cb);
    lv_indev_set_display(s_touch, s_disp);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL %d.%d.%d up on %dx%d",
             lv_version_major(), lv_version_minor(), lv_version_patch(),
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

lv_display_t *bsp_lvgl_display(void) { return s_disp; }

bool bsp_lvgl_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_lvgl_unlock(void)              { lvgl_port_unlock(); }
