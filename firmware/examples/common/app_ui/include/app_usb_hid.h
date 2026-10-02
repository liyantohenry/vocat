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

/* Keyboard modifier bits (first byte of the HID keyboard report) */
#define APP_USB_MOD_CTRL    0x01
#define APP_USB_MOD_SHIFT   0x02
#define APP_USB_MOD_ALT     0x04
#define APP_USB_MOD_GUI     0x08    /* Windows / Command / Super */

typedef enum {
    APP_USB_HID_STATE_WAITING = 0,  /* still on USB-Serial-JTAG; the keyboard starts after a delay */
    APP_USB_HID_STATE_ACTIVE,       /* keyboard installed (the host may or may not have enumerated it) */
    APP_USB_HID_STATE_JTAG,         /* USB-Serial-JTAG kept or handed back, until the next boot */
} app_usb_hid_state_t;

/** @brief Handler for "vocat ..." lines on the CDC port that app_usb_hid doesn't handle. Runs in the TinyUSB task. */
typedef void (*app_usb_hid_command_cb_t)(const char *line);

#if BOARD_ENABLE_QUICK_KEYS

/** @brief Add a handler for other "vocat ..." commands (development hooks such as switching pages). Up to 4. */
esp_err_t app_usb_hid_add_command_cb(app_usb_hid_command_cb_t cb);

/**
 * @brief Start the USB keyboard task. The keyboard itself is installed a few seconds later, unless
 *        the screen is being touched at that moment (then USB-Serial-JTAG stays until the next boot).
 */
esp_err_t app_usb_hid_init(void);

/** @brief Queue a key chord: press the modifiers and key together, then release. keycode 0 = modifiers only. */
esp_err_t app_usb_hid_send_chord(uint8_t modifiers, uint8_t keycode);

/** @brief Queue a Consumer Control usage (HID page 0x0C, e.g. 0xCD play/pause): press, then release. */
esp_err_t app_usb_hid_send_consumer(uint16_t usage);

/** @brief Queue tearing the keyboard down and routing USB back to USB-Serial-JTAG until the next boot. */
esp_err_t app_usb_hid_release_to_jtag(void);

app_usb_hid_state_t app_usb_hid_get_state(void);

/** @brief True when the keyboard is installed and the host has configured it. */
bool app_usb_hid_host_ready(void);

/** @brief Feed touch press/release so a finger held on the screen at start-up keeps USB-Serial-JTAG. */
void app_usb_hid_on_touch(bool pressed);

#else

static inline esp_err_t app_usb_hid_add_command_cb(app_usb_hid_command_cb_t cb) { (void)cb; return ESP_OK; }
static inline esp_err_t app_usb_hid_init(void) { return ESP_OK; }
static inline esp_err_t app_usb_hid_send_chord(uint8_t modifiers, uint8_t keycode) { (void)modifiers; (void)keycode; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t app_usb_hid_send_consumer(uint16_t usage) { (void)usage; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t app_usb_hid_release_to_jtag(void) { return ESP_OK; }
static inline app_usb_hid_state_t app_usb_hid_get_state(void) { return APP_USB_HID_STATE_JTAG; }
static inline bool app_usb_hid_host_ready(void) { return false; }
static inline void app_usb_hid_on_touch(bool pressed) { (void)pressed; }

#endif

#ifdef __cplusplus
}
#endif
