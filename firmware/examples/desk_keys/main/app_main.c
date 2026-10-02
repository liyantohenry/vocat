/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Desk keys: the desk dashboard (clock + weather + rate ticker) and the USB quick keys, without voice.
 * Wi-Fi is set up from any phone or laptop through the device's own setup network (wifi_setup.c); no
 * accounts, apps or cloud services. The keys work without Wi-Fi.
 *
 * Holding a finger on the clock page for 10 s asks whether to forget the Wi-Fi network; a second hold
 * within the next 20 s does it. Holding the POWER button for 2 s switches the board off (app_power.c).
 */

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include <esp_board_manager.h>

#include "app_dashboard.h"
#include "app_display.h"
#include "app_power.h"
#include "app_settings.h"
#include "wifi_setup.h"

static const char *TAG = "main";

#define FORGET_HOLD_US  (10LL * 1000 * 1000)
#define FORGET_ARM_US   (20LL * 1000 * 1000)    /* the second hold has to come within this */

static esp_timer_handle_t s_hold_timer;
static int64_t s_armed_until;

/* Two holds rather than one: a single long touch happens by accident when the board is picked up, and
 * losing the Wi-Fi network that way means setting it up again from a phone.
 */
static void hold_timer_cb(void *arg)
{
    if (esp_timer_get_time() < s_armed_until) {
        s_armed_until = 0;
        ESP_LOGW(TAG, "Screen held again: forgetting Wi-Fi");
        app_dashboard_notice("Wi-Fi forgotten", 10);
        wifi_setup_forget();
    } else {
        s_armed_until = esp_timer_get_time() + FORGET_ARM_US;
        ESP_LOGW(TAG, "Screen held for %lld s: asking before forgetting Wi-Fi", FORGET_HOLD_US / 1000000);
        app_dashboard_notice("Hold again to forget Wi-Fi", FORGET_ARM_US / 1000000);
    }
}

/* Display touch hook (gfx task). Touches the dashboard uses (keys, swipes) never start the hold. */
static void touch_cb(bool pressed, bool handled)
{
    esp_timer_stop(s_hold_timer);
    if (pressed && !handled) {
        esp_timer_start_once(s_hold_timer, FORGET_HOLD_US);
    }
}

void app_main(void)
{
    ESP_RETURN_VOID_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create event loop");
    ESP_RETURN_VOID_ON_ERROR(nvs_flash_init(), TAG, "Failed to initialize NVS");

    /* Name, which computer the keys are for, brightness: kept in flash */
    ESP_RETURN_VOID_ON_ERROR(app_settings_init(), TAG, "Failed to load settings");

    /* Peripherals (display, touch, backlight, audio codec) from the board definition */
    ESP_RETURN_VOID_ON_ERROR(esp_board_manager_init(), TAG, "Failed to initialize board manager");

    /* Display, dashboard, quick keys and the USB keyboard */
    ESP_RETURN_VOID_ON_ERROR(app_display_init(), TAG, "Failed to initialize display");
    app_dashboard_configure(&(app_dashboard_config_t) { .voice = false });

    const esp_timer_create_args_t hold_args = { .callback = hold_timer_cb, .name = "forget_hold" };
    ESP_RETURN_VOID_ON_ERROR(esp_timer_create(&hold_args, &s_hold_timer), TAG, "Failed to create hold timer");
    app_display_set_touch_cb(touch_cb);

    ESP_RETURN_VOID_ON_ERROR(wifi_setup_start(), TAG, "Failed to start Wi-Fi");

    /* Hold POWER to switch the board off */
    ESP_RETURN_VOID_ON_ERROR(app_power_init(), TAG, "Failed to watch the POWER button");
}
