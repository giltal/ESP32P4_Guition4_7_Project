/*
 * ESP32-C6 co-processor firmware management.
 *
 * The matching slave image is built from the same esp_hosted version this
 * project uses and embedded in the application, then pushed to the C6 over the
 * existing SDIO link. Embedded rather than downloaded because the C6 has no
 * exposed UART on the JC-ESP32P4-M3 module — there is no other way in, so the
 * update must not depend on the network being up.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push the embedded image if the C6 reports a different version.
 *
 * Call once the hosted transport is up (after bsp_wifi_init()). Does not
 * return on success — host and co-processor restart together. A no-op when the
 * versions already match.
 */
esp_err_t bsp_copro_update_if_needed(void);

/**
 * @brief Push the embedded image regardless of the reported version.
 *
 * For replacing a co-processor image that reports the right version but
 * misbehaves — the version says nothing about how it was configured. Skips the
 * failed-attempt limit too, since this only happens when someone asks for it.
 *
 * Does not return on success.
 */
esp_err_t bsp_copro_update_force(void);

#ifdef __cplusplus
}
#endif
