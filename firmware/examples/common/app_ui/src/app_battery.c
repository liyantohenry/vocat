/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Battery level from the BQ27220 fuel gauge on the shared I2C bus (BOARD_BATTERY_GAUGE_ADDR).
 *
 * The gauge works out the state of charge itself; this file only reads its standard commands
 * (little-endian 16-bit words) every READ_PERIOD_MS and keeps the last result for the dashboard. The
 * register use follows the stock CyberAI firmware, a xiaozhi esp-vocat fork that logs
 * "BQ27220 initialized (I2C 0x55)".
 */

#include "app_battery.h"

#if defined(BOARD_BATTERY_GAUGE_ADDR)

#include <driver/i2c_master.h>
#include <esp_board_manager.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "app_battery";

#define READ_PERIOD_MS      30000
#define RETRY_PERIOD_MS     5000    /* while the gauge hasn't answered yet */
#define I2C_SPEED_HZ        100000
#define I2C_TIMEOUT_MS      50

#define REG_BATTERY_STATUS  0x0A
#define REG_CURRENT         0x0C    /* mA, signed, positive while charging */
#define REG_STATE_OF_CHARGE 0x2C    /* % */

#define STATUS_DSG          (1u << 0)   /* discharging: no charger */
#define CHARGING_MA         30

static i2c_master_dev_handle_t s_dev;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_battery_state_t s_state;

static esp_err_t read_word(uint8_t reg, int16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *value = (int16_t)(data[0] | (data[1] << 8));
    return ESP_OK;
}

static void publish(bool valid, int percent, bool on_usb)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state.valid != valid || s_state.percent != percent || s_state.on_usb != on_usb) {
        s_state.valid = valid;
        s_state.percent = percent;
        s_state.on_usb = on_usb;
        s_state.version++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void battery_task(void *arg)
{
    (void)arg;
    bool logged = false, failed = false;
    while (true) {
        int16_t soc = 0, status = 0, current = 0;
        bool ok = read_word(REG_STATE_OF_CHARGE, &soc) == ESP_OK &&
                  read_word(REG_BATTERY_STATUS, &status) == ESP_OK &&
                  read_word(REG_CURRENT, &current) == ESP_OK;
        if (ok) {
            int percent = soc < 0 ? 0 : soc > 100 ? 100 : soc;
            /* Not discharging means a charger is attached; current is the fallback, as in the stock firmware */
            bool on_usb = ((uint16_t)status & STATUS_DSG) == 0 || current > CHARGING_MA;
            if (!logged) {
                ESP_LOGI(TAG, "Battery %d%%, status 0x%04x, %d mA", percent, (uint16_t)status, current);
                logged = true;
            }
            publish(true, percent, on_usb);
        } else {
            if (!failed) {
                ESP_LOGW(TAG, "The battery gauge doesn't answer at 0x%02x", BOARD_BATTERY_GAUGE_ADDR);
                failed = true;
            }
            publish(false, 0, false);
        }
        vTaskDelay(pdMS_TO_TICKS(ok || logged ? READ_PERIOD_MS : RETRY_PERIOD_MS));
    }
}

esp_err_t app_battery_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_periph_handle("i2c_master", (void **)&bus), TAG, "No I2C bus");

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_BATTERY_GAUGE_ADDR,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "Failed to add the gauge");
    ESP_RETURN_ON_FALSE(xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create battery task");
    return ESP_OK;
}

void app_battery_get(app_battery_state_t *state)
{
    portENTER_CRITICAL(&s_lock);
    *state = s_state;
    portEXIT_CRITICAL(&s_lock);
}

#endif
