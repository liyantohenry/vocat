/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the (hidden) Neurons page. Call from the dashboard's init with the gfx lock held, after
 *        its backdrop so the page draws on top of it.
 */
esp_err_t app_neurons_create(void *gfx_handle);

/* The rest run in the gfx task only (dashboard ui_tick and touch callback). */

/** @brief Show or hide the page. It only animates while shown. Returns true if the page now covers the screen. */
bool app_neurons_show(bool show);

/** @brief A tap on the page: play the texts once, unless they are already playing. */
void app_neurons_tap(void);

#ifdef __cplusplus
}
#endif
