/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Switching the board off.
 *
 * The board stays powered because the bootloader hook (cyberai_power_hold) drives the power latch high
 * and pad-holds it, so it survives resets. Releasing that hold and driving the latch low switches the
 * board off, with or without USB plugged in. The POWER button is an ordinary input, active low: the
 * hardware doesn't act on it beyond switching on, so holding it to switch off is up to this file.
 *
 * Both pins come from the board definition (BOARD_POWER_BUTTON_GPIO, BOARD_POWER_LATCH_GPIO).
 */

#include "app_power.h"

#if defined(BOARD_POWER_BUTTON_GPIO) && defined(BOARD_POWER_LATCH_GPIO)

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/rtc_cntl_reg.h>

#include "app_display.h"

static const char *TAG = "app_power";

#define HOLD_TO_OFF_MS  2000
#define POLL_MS         30

void app_power_off(void)
{
    ESP_LOGW(TAG, "Switching off");
    app_display_set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(150));

    REG_CLR_BIT(RTC_CNTL_PAD_HOLD_REG, 1u << BOARD_POWER_LATCH_GPIO);   /* release the bootloader's hold */
    gpio_set_level(BOARD_POWER_LATCH_GPIO, 0);

    /* The power should be gone by now. If something else keeps the board alive, carry on with a lit screen
     * rather than sit in the dark.
     */
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(BOARD_POWER_LATCH_GPIO, 1);
    REG_SET_BIT(RTC_CNTL_PAD_HOLD_REG, 1u << BOARD_POWER_LATCH_GPIO);
    app_display_set_backlight(100);
    ESP_LOGE(TAG, "Still running after releasing the power latch");
}

static void power_task(void *arg)
{
    (void)arg;
    bool armed = false;         /* the button is usually still held right after switching on */
    int64_t pressed_at = 0;

    while (true) {
        bool down = gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 0;
        if (!armed) {
            armed = !down;
        } else if (!down) {
            pressed_at = 0;
        } else if (pressed_at == 0) {
            pressed_at = esp_timer_get_time();
        } else if (esp_timer_get_time() - pressed_at >= HOLD_TO_OFF_MS * 1000LL) {
            app_power_off();
            pressed_at = 0;
            armed = false;      /* wait for a release before the next hold counts */
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t app_power_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "Failed to configure the POWER button");
    ESP_RETURN_ON_FALSE(xTaskCreate(power_task, "power", 3072, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create the power task");
    ESP_LOGI(TAG, "Hold POWER for %d s to switch off", HOLD_TO_OFF_MS / 1000);
    return ESP_OK;
}

#endif /* BOARD_POWER_BUTTON_GPIO && BOARD_POWER_LATCH_GPIO */
