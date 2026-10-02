/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Settings kept in flash: the name on the clock page, which computer the Desktop keys are for, the
 * screen brightness, and the Neurons page's texts, colour and hold time. Wi-Fi credentials are not here;
 * the Wi-Fi driver saves those itself.
 *
 * Threading: the setters only touch memory, so the gfx task and the web handlers can call them. The
 * write to flash happens in settings_task, which also waits a moment so a run of changes (dragging the
 * brightness slider, say) becomes one write.
 */

#include "app_settings.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

static const char *TAG = "app_settings";

#define NVS_NAMESPACE   "vocat"
#define KEY_NAME        "name"
#define KEY_OS          "os"
#define KEY_BRIGHTNESS  "bright"
#define KEY_TEXT_FMT    "text%d"
#define KEY_NCOLOR      "ncolor"
#define KEY_NHOLD       "nhold"
#define SAVE_DELAY_MS   800
#define BRIGHTNESS_MIN  10

static struct {
    char name[APP_SETTINGS_NAME_MAX + 1];
    uint8_t os;
    uint8_t brightness;
    char text[APP_SETTINGS_TEXT_COUNT][APP_SETTINGS_NAME_MAX + 1];
    uint32_t neuron_color;
    uint8_t neuron_hold;
} s_cfg = {
    .name = "", .os = APP_SETTINGS_OS_UNSET, .brightness = 100,
    .neuron_color = APP_SETTINGS_NEURON_COLOR_DEFAULT, .neuron_hold = APP_SETTINGS_NEURON_HOLD_DEFAULT,
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile uint32_t s_version;

static void changed(void)
{
    s_version++;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void settings_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(SAVE_DELAY_MS));
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_DELAY_MS)) > 0) {
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        typeof(s_cfg) cfg = s_cfg;
        xSemaphoreGive(s_lock);

        nvs_handle_t nvs;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            nvs_set_str(nvs, KEY_NAME, cfg.name);
            nvs_set_u8(nvs, KEY_OS, cfg.os);
            nvs_set_u8(nvs, KEY_BRIGHTNESS, cfg.brightness);
            for (int i = 0; i < APP_SETTINGS_TEXT_COUNT; i++) {
                char key[8];
                snprintf(key, sizeof(key), KEY_TEXT_FMT, i + 1);
                nvs_set_str(nvs, key, cfg.text[i]);
            }
            nvs_set_u32(nvs, KEY_NCOLOR, cfg.neuron_color);
            nvs_set_u8(nvs, KEY_NHOLD, cfg.neuron_hold);
            err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Saved: name \"%s\", keys for %s, brightness %u%%, neurons #%06" PRIX32 " %u s (%s)", cfg.name,
                 app_settings_os_name(cfg.os), cfg.brightness, cfg.neuron_color, cfg.neuron_hold, esp_err_to_name(err));
    }
}

esp_err_t app_settings_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "Out of memory");

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_cfg.name);
        if (nvs_get_str(nvs, KEY_NAME, s_cfg.name, &len) != ESP_OK) {
            s_cfg.name[0] = '\0';
        }
        uint8_t v;
        if (nvs_get_u8(nvs, KEY_OS, &v) == ESP_OK && v <= APP_SETTINGS_OS_UNSET) {
            s_cfg.os = v;
        }
        if (nvs_get_u8(nvs, KEY_BRIGHTNESS, &v) == ESP_OK && v >= BRIGHTNESS_MIN && v <= 100) {
            s_cfg.brightness = v;
        }
        for (int i = 0; i < APP_SETTINGS_TEXT_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), KEY_TEXT_FMT, i + 1);
            len = sizeof(s_cfg.text[i]);
            if (nvs_get_str(nvs, key, s_cfg.text[i], &len) != ESP_OK) {
                s_cfg.text[i][0] = '\0';
            }
        }
        uint32_t color;
        if (nvs_get_u32(nvs, KEY_NCOLOR, &color) == ESP_OK && color <= 0xFFFFFF) {
            s_cfg.neuron_color = color;
        }
        if (nvs_get_u8(nvs, KEY_NHOLD, &v) == ESP_OK && v >= APP_SETTINGS_NEURON_HOLD_MIN && v <= APP_SETTINGS_NEURON_HOLD_MAX) {
            s_cfg.neuron_hold = v;
        }
        nvs_close(nvs);
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(settings_task, "settings", 4096, NULL, 2, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create the settings task");
    ESP_LOGI(TAG, "Name \"%s\", keys for %s, brightness %u%%", s_cfg.name,
             app_settings_os_name(s_cfg.os), s_cfg.brightness);
    return ESP_OK;
}

void app_settings_get_name(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!s_lock) {
        out[0] = '\0';
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_cfg.name, out_size);
    xSemaphoreGive(s_lock);
}

void app_settings_get_text(int index, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!s_lock || index < 0 || index >= APP_SETTINGS_TEXT_COUNT) {
        out[0] = '\0';
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_cfg.text[index], out_size);
    xSemaphoreGive(s_lock);
}

uint32_t app_settings_get_neuron_color(void)
{
    return s_cfg.neuron_color;
}

int app_settings_get_neuron_hold(void)
{
    return s_cfg.neuron_hold;
}

app_settings_os_t app_settings_get_os(void)
{
    return (app_settings_os_t)s_cfg.os;
}

int app_settings_get_brightness(void)
{
    return s_cfg.brightness;
}

const char *app_settings_os_name(app_settings_os_t os)
{
    switch (os) {
        case APP_SETTINGS_OS_WINDOWS: return "Windows";
        case APP_SETTINGS_OS_MACOS:   return "Mac";
        case APP_SETTINGS_OS_LINUX:   return "Linux";
        default:                      return "not set";
    }
}

esp_err_t app_settings_set_name(const char *name)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.name, name ? name : "", sizeof(s_cfg.name));
    xSemaphoreGive(s_lock);
    changed();
    return ESP_OK;
}

esp_err_t app_settings_set_os(app_settings_os_t os)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(os <= APP_SETTINGS_OS_UNSET, ESP_ERR_INVALID_ARG, TAG, "Unknown OS");
    s_cfg.os = (uint8_t)os;
    changed();
    return ESP_OK;
}

esp_err_t app_settings_set_brightness(int percent)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    if (percent < BRIGHTNESS_MIN) {
        percent = BRIGHTNESS_MIN;
    } else if (percent > 100) {
        percent = 100;
    }
    s_cfg.brightness = (uint8_t)percent;
    changed();
    return ESP_OK;
}

esp_err_t app_settings_set_text(int index, const char *text)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(index >= 0 && index < APP_SETTINGS_TEXT_COUNT, ESP_ERR_INVALID_ARG, TAG, "No text %d", index);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.text[index], text ? text : "", sizeof(s_cfg.text[index]));
    xSemaphoreGive(s_lock);
    changed();
    return ESP_OK;
}

esp_err_t app_settings_set_neuron_color(uint32_t rgb)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    s_cfg.neuron_color = rgb & 0xFFFFFF;
    changed();
    return ESP_OK;
}

esp_err_t app_settings_set_neuron_hold(int seconds)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    if (seconds < APP_SETTINGS_NEURON_HOLD_MIN) {
        seconds = APP_SETTINGS_NEURON_HOLD_MIN;
    } else if (seconds > APP_SETTINGS_NEURON_HOLD_MAX) {
        seconds = APP_SETTINGS_NEURON_HOLD_MAX;
    }
    s_cfg.neuron_hold = (uint8_t)seconds;
    changed();
    return ESP_OK;
}

uint32_t app_settings_version(void)
{
    return s_version;
}
