idf_component_register(
    SRCS "bsp.c" "bsp_sdcard.c" "bsp_wifi.c" "bsp_wifi_prov.c" "bsp_wifi_store.c"
         "bsp_ble.c" "bsp_ota.c" "bsp_ota_web.c" "bsp_lvgl.c" "bsp_remote.c"
         "bsp_screenshot.c" "bsp_copro.c"
    INCLUDE_DIRS "include"
    REQUIRES st7701_lcd gt911_touch ppa_engine audio
             esp_driver_i2c esp_driver_ledc esp_driver_sdmmc
             fatfs vfs sdmmc esp_psram
             # Radio: esp_wifi_remote forwards esp_wifi_* to the C6 over
             # ESP-Hosted; bt supplies the NimBLE host. esp_wifi_remote is a
             # managed component, hence the namespaced name.
             espressif__esp_wifi_remote esp_netif esp_event esp_timer nvs_flash bt
             # OTA update
             app_update esp_https_ota esp_http_client esp_app_format
             esp_partition esp-tls mbedtls esp_http_server lwip
             # GUI
             lvgl__lvgl espressif__esp_lvgl_port
             # Co-processor firmware management
             espressif__esp_hosted

    # ESP32-C6 slave image pushed by bsp_copro.c. Embedded rather than fetched:
    # the C6 has no exposed UART on this module, so the update path must not
    # depend on the network.
    EMBED_FILES "network_adapter.bin"
)
