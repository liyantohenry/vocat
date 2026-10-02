/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#include "board_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#if BOARD_ENABLE_QUICK_KEYS

/* Key pages shown by the dashboard: media, desktop */
#define APP_QUICK_KEYS_PAGE_COUNT 2

/**
 * @brief Create the (hidden) key widgets and start the USB keyboard. Call from the dashboard's init
 *        with the gfx lock held, after its backdrop so the keys draw on top of it.
 */
esp_err_t app_quick_keys_create(void *gfx_handle);

/* The rest run in the gfx task only (dashboard ui_tick and touch callback). */

/** @brief Show key page `page` (0 .. APP_QUICK_KEYS_PAGE_COUNT - 1), or hide the keys with -1. */
void app_quick_keys_show(int page);

/** @brief A touch went down on the shown key page: highlight the key under it. */
void app_quick_keys_on_press(uint16_t x, uint16_t y);

/** @brief The touch was released. A swipe only clears the highlight; otherwise the key fires. */
void app_quick_keys_on_release(uint16_t x, uint16_t y, bool swiped);

/** @brief Periodic refresh of the status line (USB state, host OS). */
void app_quick_keys_tick(void);

#endif

#ifdef __cplusplus
}
#endif
