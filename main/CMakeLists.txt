idf_component_register(
    SRCS "main.c" "ui.c" "ble_control.c" "ble_uart.c" "ble_console.c"
    INCLUDE_DIRS "."
    REQUIRES bsp st7701_lcd gt911_touch ppa_engine pngaux
             app_update esp_partition esp_app_format nvs_flash
             lvgl__lvgl espressif__esp_lvgl_port
             bt
)
