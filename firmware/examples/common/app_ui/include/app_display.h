/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "expression_emote.h"
#include <esp_lcd_panel_io.h>
#include "app_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the display
 *
 * To check the provision status, should be called after setup_rainmaker_init() and before setup_rainmaker_start().
 *
 * @param panel_handle The panel handle
 * @param io_handle The io handle
 * @param h_res The horizontal resolution
 * @param v_res The vertical resolution
 * @param touch_handle The touch handle, can be NULL if not supported
 * @param event_cb The callback function for touch events
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_display_init();

/**
 * @brief Set text to display based on text type
 *
 * @param text_type The type of text (user, assistant, or system)
 * @param text The text to display, can be NULL to clear
 * @param arg User argument (unused)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_display_set_text(app_device_text_type_t text_type, const char *text, void *arg);

/**
 * @brief Handle system state changes
 *
 * @param new_state The new system state (sleep, active, or listening)
 * @param arg User argument (unused)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_display_system_state_changed(app_device_system_state_t new_state, void *arg);

/**
 * @brief Emotion string constants, these depend on the assets in the display firmware.
 *
 * @note These should be updated if the assets are changed.
 */
#define DISP_EMOTE_NEUTRAL "neutral"
#define DISP_EMOTE_HAPPY "happy"
#define DISP_EMOTE_SAD "sad"
#define DISP_EMOTE_CRYING "crying"
#define DISP_EMOTE_ANGRY "angry"
#define DISP_EMOTE_SLEEPY "sleepy"
#define DISP_EMOTE_CONFUSED "confused"
#define DISP_EMOTE_SHOCKED "shocked"
#define DISP_EMOTE_WINKING "winking"
#define DISP_EMOTE_IDLE "idle"

/** @brief Set the backlight, 0 to 100 percent (boards with LEDC_BACKLIGHT_SUPPORTED). */
esp_err_t app_display_set_backlight(int percent);

/**
 * @brief Touch hook, called in the gfx task for every press and release after the dashboard has seen it.
 *
 * @param pressed  true for a press, false for a release
 * @param handled  true if the dashboard used the touch (a swipe, a key, the volume row)
 */
typedef void (*app_display_touch_cb_t)(bool pressed, bool handled);

/** @brief Set the touch hook (the voice app uses it for tap-to-interrupt and the factory-reset hold). */
void app_display_set_touch_cb(app_display_touch_cb_t cb);

/**
 * @brief Show or hide the emote face. A full-screen dashboard page hides it, so the gfx task doesn't
 *        decode the face's animation under the page every frame. gfx task only.
 */
void app_display_set_face_visible(bool visible);

/** @brief Show a QR code on the emote screen in place of the face, with `caption` as the system text. */
esp_err_t app_display_show_qrcode(const char *text, const char *caption);

/** @brief Hide the QR code and show the idle face again. */
void app_display_hide_qrcode(void);

bool app_display_is_emotion_valid(const char *emotion);

/**
 * @brief Set the emotion to display, should be one of DISP_EMOTE_XXX macros
 *
 * @param emotion The emotion to display
 * @return ESP_OK on success, if the emotion is not valid, return ESP_ERR_INVALID_ARG
 */
esp_err_t app_display_set_emotion(const char *emotion);

#ifdef __cplusplus
}
#endif
