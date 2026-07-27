/*
 * BLE GATT service exposing the shared control state.
 *
 * The same mode/level/led/message that the web page and the touchscreen edit,
 * reachable from a phone with no WiFi involved. Each field is its own
 * characteristic — readable, writable, and notifying on change — so a student
 * can poke at them from any generic BLE app without needing a decoder.
 *
 * Service   4f3a0001-9d2b-4c7e-a1f5-8b6d3e0c7a21
 *   mode    4f3a0002-...   uint8   0=Idle 1=Run 2=Test
 *   level   4f3a0003-...   uint8   0-100
 *   led     4f3a0004-...   uint8   0 or 1
 *   message 4f3a0005-...   utf8    up to 32 bytes
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 16 bytes, most significant first — pass to bsp_ble_config_t.adv_uuid128. */
extern const uint8_t ble_control_service_uuid[16];

/**
 * @brief Register the service with NimBLE.
 *
 * Pass as bsp_ble_config_t.register_services; it must run in the window
 * before the host task starts.
 */
void ble_control_register(void);

#ifdef __cplusplus
}
#endif
