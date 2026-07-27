/*
 * Board pin map — Guition JC4880P443C-I-W
 *
 * ESP32-P4 + ESP32-C6 (radio via ESP-Hosted over SDIO)
 * 4.3" 480x800 ST7701S MIPI-DSI panel, GT911 touch, ES8311 audio, SDMMC slot 0.
 */
#pragma once

/* ── Display (ST7701S MIPI-DSI) ─────────────────────────────────── */
#define BSP_LCD_H_RES           480
#define BSP_LCD_V_RES           800
#define BSP_LCD_RST_GPIO        (-1)    /* not wired */
#define BSP_LCD_BK_LIGHT_GPIO   23      /* LEDC-driven backlight */

/* ── I2C bus (shared: GT911 touch + ES8311 codec) ───────────────── */
/* Port 1 is mandatory: gt911_touch_init() looks the bus up by port number. */
#define BSP_I2C_PORT            1
#define BSP_I2C_SDA_GPIO        7
#define BSP_I2C_SCL_GPIO        8

/* ── Touch (GT911) ──────────────────────────────────────────────── */
/* RST/INT are not connected on this board — do not drive them. */
#define BSP_TOUCH_RST_GPIO      (-1)
#define BSP_TOUCH_INT_GPIO      (-1)

/* ── Audio (ES8311 over I2S) ────────────────────────────────────── */
#define BSP_I2S_MCLK_GPIO       13
#define BSP_I2S_BCLK_GPIO       12
#define BSP_I2S_WS_GPIO         10
#define BSP_I2S_DOUT_GPIO        9      /* P4 -> ES8311 SDIN */
#define BSP_I2S_DIN_GPIO        48      /* ES8311 DOUT -> P4 */
#define BSP_AUDIO_PA_GPIO       11      /* power amp enable, active high */

/* ── SD card (SDMMC slot 0, 4-bit, on-chip LDO channel 4) ───────── */
#define BSP_SD_CLK_GPIO         43
#define BSP_SD_CMD_GPIO         44
#define BSP_SD_D0_GPIO          39
#define BSP_SD_D1_GPIO          40
#define BSP_SD_D2_GPIO          41
#define BSP_SD_D3_GPIO          42
#define BSP_SD_LDO_CHAN         4
#define BSP_SD_MOUNT_POINT      "/sd"

/* ── Radio (ESP32-C6 via ESP-Hosted, SDIO slot 1) ───────────────── */
/*
 * The C6 is inside the JC-ESP32P4-M3 module, so this bus is module-internal
 * wiring rather than board wiring — it is identical on every product built on
 * that module (confirmed against the JC-ESP32P4-M3 reference and matching the
 * 4.7" JC1060P470C board in ESP32P4_7_HebClock).
 *
 * Slot 1 is mandatory: slot 0 is taken by the SD card above.
 *
 * These defines document the wiring. ESP-Hosted itself is configured through
 * the CONFIG_ESP_HOSTED_* symbols in sdkconfig.defaults — keep the two in sync.
 */
#define BSP_C6_RST_GPIO         54
#define BSP_SDIO_CLK_GPIO       18
#define BSP_SDIO_CMD_GPIO       19
#define BSP_SDIO_D0_GPIO        14
#define BSP_SDIO_D1_GPIO        15
#define BSP_SDIO_D2_GPIO        16
#define BSP_SDIO_D3_GPIO        17
