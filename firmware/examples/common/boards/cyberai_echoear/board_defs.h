/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* CyberAI EchoEar clone.
 * No indicator LED (GPIO43 is a UART TX line on this board), so INDICATOR_DEVICE_NAME is not defined.
 */

/* The head capacitive touch pad of the official board (GPIO7) is not present:
 * GPIO7 is one of the power/enable outputs held high by the bootloader hook.
 */
#define CAPACITIVE_TOUCH_SUPPORTED 0

/* Whether the board supports LEDC backlight */
#define LEDC_BACKLIGHT_SUPPORTED 1

/* Provisioning QR code image size in pixels (the widget default is 100 px, about 1 cm on this panel).
 * 210 px gives a 37-module code at 5 px per module plus a ~12 px white quiet zone.
 */
#define BOARD_QRCODE_DISPLAY_SIZE 210

/* Skip the interactive `esp>` console (agent_console). It installs the USB-Serial-JTAG driver,
 * and while a USB host is attached without the serial port open, its console I/O stalls the
 * app, freezing the display on a blank frame. Plain log output still goes to USB-Serial-JTAG.
 */
#define BOARD_DISABLE_INTERACTIVE_CONSOLE 1

/* Switching off (app_power.c): the POWER button is an ordinary active-low input, and driving the power
 * latch low cuts the power, with or without USB. The bootloader hook holds the latch high; app_power.c
 * releases that hold to switch off. Both were found by watching the free pins while the button was pressed.
 */
#define BOARD_POWER_BUTTON_GPIO 16
#define BOARD_POWER_LATCH_GPIO  7

/* BQ27220 fuel gauge on the shared I2C bus (app_battery.c), as in the stock CyberAI firmware */
#define BOARD_BATTERY_GAUGE_ADDR 0x55

/* Desk dashboard (app_dashboard.c): clock + weather, timers, conversation view and a rate ticker */
#define BOARD_ENABLE_DASHBOARD 1

/* Quick keys (app_quick_keys.c): Media and Desktop key pages in the dashboard, sent to the computer as a
 * USB keyboard (app_usb_hid.c). The keyboard takes over the USB port 8 s after boot, which removes
 * USB-Serial-JTAG; flash_cyberai_jtag.sh hands it back first. Needs BOARD_ENABLE_DASHBOARD.
 */
#define BOARD_ENABLE_QUICK_KEYS 1

/* The URL of the guide for this board */
#define BOARD_DEVICE_MANUAL_URL "https://raw.githubusercontent.com/espressif/esp_agents_firmware/refs/heads/main/examples/common/boards/esp_vocat_board_v1_2/README.md"
