/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include "board_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BOARD_POWER_BUTTON_GPIO) && defined(BOARD_POWER_LATCH_GPIO)

/**
 * @brief Watch the POWER button: holding it switches the board off. The button is usually still held
 *        just after switching on, so the watch only arms once it has been released.
 */
esp_err_t app_power_init(void);

/** @brief Switch the board off: blank the screen, then release the power latch. Doesn't return. */
void app_power_off(void);

#else

static inline esp_err_t app_power_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void app_power_off(void) { }

#endif

#ifdef __cplusplus
}
#endif
