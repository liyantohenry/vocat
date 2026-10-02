/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Types the UI shares with the voice device state machine (app_device.h in app_common) */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_DEVICE_TEXT_TYPE_USER,
    APP_DEVICE_TEXT_TYPE_ASSISTANT,
    APP_DEVICE_TEXT_TYPE_SYSTEM,
} app_device_text_type_t;

typedef enum {
    APP_DEVICE_SYSTEM_STATE_SLEEP,
    APP_DEVICE_SYSTEM_STATE_ACTIVE,
    APP_DEVICE_SYSTEM_STATE_LISTENING,
} app_device_system_state_t;

#ifdef __cplusplus
}
#endif
