/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Settings kept in flash (NVS namespace "vocat"). Wi-Fi credentials are not here: the Wi-Fi driver
 * saves those itself, and they survive reboots, power-off and an application flash.
 */

#define APP_SETTINGS_NAME_MAX   32      /* characters of the name shown on the clock page */
#define APP_SETTINGS_TEXT_COUNT 4       /* texts the Neurons page shows after the name */
#define APP_SETTINGS_NEURON_COLOR_DEFAULT   0x5EC8F2
#define APP_SETTINGS_NEURON_HOLD_DEFAULT    5
#define APP_SETTINGS_NEURON_HOLD_MIN        1
#define APP_SETTINGS_NEURON_HOLD_MAX        15

typedef enum {
    APP_SETTINGS_OS_WINDOWS = 0,
    APP_SETTINGS_OS_MACOS,
    APP_SETTINGS_OS_LINUX,      /* GNOME defaults */
    APP_SETTINGS_OS_COUNT,
    APP_SETTINGS_OS_UNSET = APP_SETTINGS_OS_COUNT,
} app_settings_os_t;

/** @brief Load the settings from flash. Call once, early, from a task with an internal-RAM stack. */
esp_err_t app_settings_init(void);

/** @brief Copy the device name (empty when unset). Any task. */
void app_settings_get_name(char *out, size_t out_size);

/** @brief Which computer the Desktop keys are for, or APP_SETTINGS_OS_UNSET. */
app_settings_os_t app_settings_get_os(void);

/** @brief Screen brightness in percent (10 to 100). */
int app_settings_get_brightness(void);

/** @brief Copy Neurons text `index` (0 .. APP_SETTINGS_TEXT_COUNT - 1, empty when unset). Any task. */
void app_settings_get_text(int index, char *out, size_t out_size);

/** @brief Colour of the Neurons page, 0xRRGGBB. */
uint32_t app_settings_get_neuron_color(void);

/** @brief Seconds the Neurons page holds each text (APP_SETTINGS_NEURON_HOLD_MIN to _MAX). */
int app_settings_get_neuron_hold(void);

/** @brief Name of an OS for people to read ("Windows", "Mac", "Linux"). */
const char *app_settings_os_name(app_settings_os_t os);

/* The setters return once the value is in memory; writing it to flash happens in the settings task, so
 * they are safe to call from the gfx task and from a web handler.
 */
esp_err_t app_settings_set_name(const char *name);
esp_err_t app_settings_set_os(app_settings_os_t os);
esp_err_t app_settings_set_brightness(int percent);
esp_err_t app_settings_set_text(int index, const char *text);
esp_err_t app_settings_set_neuron_color(uint32_t rgb);
esp_err_t app_settings_set_neuron_hold(int seconds);

/** @brief Counter that changes whenever a setting changes, for screens that redraw on change. */
uint32_t app_settings_version(void);

#ifdef __cplusplus
}
#endif
