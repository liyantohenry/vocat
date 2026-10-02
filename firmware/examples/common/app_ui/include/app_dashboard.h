/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#include "app_ui_types.h"
#include "board_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#if BOARD_ENABLE_DASHBOARD

typedef struct {
    /* Voice device: the timers and conversation pages are in the swipe cycle, and the dashboard stays
     * hidden until the device has an IP address (so the provisioning QR code on the emote screen shows).
     * Otherwise the dashboard shows from boot with the clock and key pages only.
     */
    bool voice;
    /* Speaker volume row on the timers page; leave both NULL for none */
    uint8_t (*get_volume)(void);
    esp_err_t (*set_volume)(uint8_t volume);
} app_dashboard_config_t;

/**
 * @brief Configure the dashboard. Until this is called it behaves as `voice` with no volume row. Call it
 *        right after app_display_init().
 */
void app_dashboard_configure(const app_dashboard_config_t *config);

/**
 * @brief Show a line at the bottom of the clock page for `seconds` (the board's address once it is
 *        online, say). NULL clears it. Any task.
 */
void app_dashboard_notice(const char *text, int seconds);

/**
 * @brief Show Wi-Fi setup on the clock page: a QR code that joins the open network `ssid`, and the
 *        `address` of the setup page to open once joined. ssid NULL shows the clock again. Any task.
 */
void app_dashboard_set_wifi_setup(const char *ssid, const char *address);

/**
 * @brief Create the desk dashboard (clock + weather, timers, conversation view, rate ticker)
 *        on top of the emote UI. It stays hidden until the device has an IP address.
 *
 * @param gfx_handle The gfx handle used by the emote UI
 */
esp_err_t app_dashboard_init(void *gfx_handle);

/** @brief Follow the device state: listening switches to the conversation view, sleep returns to the clock. */
void app_dashboard_on_state(app_device_system_state_t state);

/** @brief Show user/assistant/system text in the conversation view. */
void app_dashboard_on_text(app_device_text_type_t type, const char *text);

/**
 * @brief Feed touch events for swipes, the volume row and the quick-key pages.
 *
 * @return true if the dashboard handled the touch: for a press, it landed on a key page or the Neurons
 *         page (don't start the hold timers); for a release, it was a swipe or a tap the dashboard used
 *         (don't treat it as a tap for the agent)
 */
bool app_dashboard_on_touch(bool pressed, uint16_t x, uint16_t y);

/** @brief Show a countdown for a reminder set by the agent. */
void app_dashboard_add_timer(const char *label, int seconds);

#else

typedef struct {
    bool voice;
    uint8_t (*get_volume)(void);
    esp_err_t (*set_volume)(uint8_t volume);
} app_dashboard_config_t;

static inline void app_dashboard_configure(const app_dashboard_config_t *config) { (void)config; }
static inline void app_dashboard_notice(const char *text, int seconds) { (void)text; (void)seconds; }
static inline void app_dashboard_set_wifi_setup(const char *ssid, const char *address) { (void)ssid; (void)address; }
static inline esp_err_t app_dashboard_init(void *gfx_handle) { (void)gfx_handle; return ESP_OK; }
static inline void app_dashboard_on_state(app_device_system_state_t state) { (void)state; }
static inline void app_dashboard_on_text(app_device_text_type_t type, const char *text) { (void)type; (void)text; }
static inline bool app_dashboard_on_touch(bool pressed, uint16_t x, uint16_t y) { (void)pressed; (void)x; (void)y; return false; }
static inline void app_dashboard_add_timer(const char *label, int seconds) { (void)label; (void)seconds; }

#endif

#ifdef __cplusplus
}
#endif
