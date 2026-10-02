/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>

esp_err_t app_touch_press_init(void);

bool app_touch_press_on_active(void);

bool app_touch_press_on_inactive(void);

/** @brief Stop long-press tracking without treating the release as a tap (e.g. after a swipe). */
void app_touch_press_cancel(void);

void app_touch_press_deinit(void);

