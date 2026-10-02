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

typedef struct {
    bool valid;         /* false until the gauge answers, and again if it stops answering */
    int percent;        /* state of charge, 0-100 */
    bool on_usb;        /* charging, or full and held there by USB */
    uint32_t version;   /* changes whenever the fields above change */
} app_battery_state_t;

#if defined(BOARD_BATTERY_GAUGE_ADDR)

/** @brief Start reading the battery gauge every 30 s in a task of its own. Needs the board manager. */
esp_err_t app_battery_init(void);

/** @brief The last reading. Cheap and safe from any task, the gfx task included. */
void app_battery_get(app_battery_state_t *state);

#else

static inline esp_err_t app_battery_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void app_battery_get(app_battery_state_t *state) { *state = (app_battery_state_t){ 0 }; }

#endif

#ifdef __cplusplus
}
#endif
