/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB keyboard for the quick keys: a TinyUSB composite device with one HID interface (keyboard plus
 * consumer control, report IDs 1 and 2) and one CDC-ACM control port.
 *
 * The ESP32-S3 has a single full-speed USB PHY. Installing TinyUSB routes it to the USB-OTG controller,
 * so USB-Serial-JTAG, the only way to flash this board, disappears while the keyboard runs. The ways back:
 *  - every boot starts on USB-Serial-JTAG (any reset cuts this board's power), and the keyboard is only
 *    installed START_DELAY_MS later;
 *  - a finger held on the screen when that delay ends keeps USB-Serial-JTAG for this boot;
 *  - "vocat usb-release" on the CDC port tears TinyUSB down and routes the PHY back to USB-Serial-JTAG
 *    until the next boot. scripts/flash_cyberai_jtag.sh sends it before flashing.
 * While the keyboard runs and a host has the CDC port open, log output goes to the CDC port instead.
 *
 * All TinyUSB calls except the CDC receive callback run in usb_hid_task, fed by s_queue.
 */

#include "app_usb_hid.h"

#if BOARD_ENABLE_QUICK_KEYS

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/usb_serial_jtag_hal.h>

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "app_usb_hid";

#define START_DELAY_MS          8000
#define KEY_HOLD_MS             20      /* time between the press and release reports */
#define HID_READY_TIMEOUT_MS    100
#define WAKEUP_TIMEOUT_MS       1000
#define RELEASE_COMMAND         "vocat usb-release"
#define PING_COMMAND            "vocat ping"

/* Interfaces, endpoints and report IDs */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL,
};

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_CONSUMER,
};

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_HID,
    STR_COUNT,
};

#define EPNUM_CDC_NOTIF         0x81
#define EPNUM_CDC_OUT           0x02
#define EPNUM_CDC_IN            0x82
#define EPNUM_HID               0x83

/* Espressif VID with the PID TinyUSB's default descriptors give a CDC + HID device */
#define USB_PID                 0x4005

static const uint8_t s_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

#define CONFIG_TOTAL_LEN        (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STR_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(s_report_desc), EPNUM_HID, 16, 5),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD composite, needed for the two-interface CDC function */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static char s_serial[16];

static const char *s_strings[STR_COUNT] = {
    [STR_LANGID] = (const char[]) { 0x09, 0x04 },  /* English (0x0409) */
    [STR_MANUFACTURER] = "CyberAI",
    [STR_PRODUCT] = "VoCat Quick Keys",
    [STR_SERIAL] = s_serial,
    [STR_CDC] = "VoCat control",
    [STR_HID] = "VoCat keys",
};

typedef enum {
    REQ_CHORD,
    REQ_CONSUMER,
    REQ_RELEASE,
} req_type_t;

typedef struct {
    req_type_t type;
    uint8_t modifiers;
    uint8_t keycode;
    uint16_t usage;
} req_t;

static QueueHandle_t s_queue;
static volatile app_usb_hid_state_t s_state = APP_USB_HID_STATE_WAITING;
static volatile bool s_touch_held;

/* CDC command line, only touched in the TinyUSB task (receive callback) */
static char s_line[96];      /* fits "vocat set text <n> " and a 32-character UTF-8 text */
static size_t s_line_len;
#define MAX_COMMAND_CBS 4
static app_usb_hid_command_cb_t s_command_cbs[MAX_COMMAND_CBS];

/* Log mirroring */
static vprintf_like_t s_orig_vprintf;
static SemaphoreHandle_t s_log_lock;
static char s_log_buf[256];

/* ------------------------------------------------------------------------------------------------
 * TinyUSB callbacks
 * ---------------------------------------------------------------------------------------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* Keyboard LED output reports (caps lock etc.) are ignored */
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static void cdc_write_str(const char *s)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, strlen(s));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static void handle_command(const char *line)
{
    if (strcmp(line, RELEASE_COMMAND) == 0) {
        cdc_write_str("vocat releasing usb\r\n");
        req_t req = { .type = REQ_RELEASE };
        xQueueSend(s_queue, &req, 0);
    } else if (strcmp(line, PING_COMMAND) == 0) {
        cdc_write_str("vocat pong\r\n");
    } else if (strncmp(line, "vocat ", 6) == 0) {
        for (int i = 0; i < MAX_COMMAND_CBS && s_command_cbs[i]; i++) {
            s_command_cbs[i](line);
        }
    }
    /* Anything else (e.g. a modem manager probing the port) is ignored */
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[64];
    size_t len = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &len) == ESP_OK && len > 0) {
        for (size_t i = 0; i < len; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (s_line_len > 0) {
                    s_line[s_line_len] = '\0';
                    handle_command(s_line);
                    s_line_len = 0;
                }
            } else if (s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Log output
 * ---------------------------------------------------------------------------------------------- */

static int log_vprintf(const char *fmt, va_list args)
{
    if (s_state != APP_USB_HID_STATE_ACTIVE || !tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
        /* s_orig_vprintf is still NULL for a moment after esp_log_set_vprintf() installs this hook */
        return s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);
    }
    /* Wait briefly for another task's write; drop the line rather than stall the caller for long */
    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    if (s_state != APP_USB_HID_STATE_ACTIVE) {
        /* release_to_jtag() ran between the check above and taking the lock */
        xSemaphoreGive(s_log_lock);
        return s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);
    }
    int len = vsnprintf(s_log_buf, sizeof(s_log_buf), fmt, args);
    if (len > 0) {
        size_t n = (size_t)len < sizeof(s_log_buf) ? (size_t)len : sizeof(s_log_buf) - 1;
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s_log_buf, n);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    xSemaphoreGive(s_log_lock);
    return len;
}

/* ------------------------------------------------------------------------------------------------
 * USB task
 * ---------------------------------------------------------------------------------------------- */

static esp_err_t install(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &s_device_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STR_COUNT;
    cfg.descriptor.full_speed_config = s_config_desc;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "TinyUSB install failed");

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
    };
    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM init failed: %s", esp_err_to_name(err));
        tinyusb_driver_uninstall();
        return err;
    }

    s_state = APP_USB_HID_STATE_ACTIVE;
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

static void release_to_jtag(void)
{
    if (s_state != APP_USB_HID_STATE_ACTIVE) {
        return;
    }
    /* Stop log mirroring before the CDC port goes away, and wait out a write already in progress */
    s_state = APP_USB_HID_STATE_JTAG;
    esp_log_set_vprintf(s_orig_vprintf);
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    xSemaphoreGive(s_log_lock);

    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    esp_err_t err = tinyusb_driver_uninstall();
    usb_serial_jtag_hal_phy_set_external(NULL, false);
    ESP_LOGW(TAG, "USB handed back to USB-Serial-JTAG until the next boot (uninstall: %s)", esp_err_to_name(err));
}

static bool wait_hid_ready(void)
{
    if (s_state != APP_USB_HID_STATE_ACTIVE || !tud_mounted()) {
        return false;
    }
    if (tud_suspended()) {
        tud_remote_wakeup();
        for (int i = 0; i < WAKEUP_TIMEOUT_MS && tud_suspended(); i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    for (int i = 0; i < HID_READY_TIMEOUT_MS; i++) {
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static void send_chord(uint8_t modifiers, uint8_t keycode)
{
    uint8_t keys[6] = { keycode };
    /* Modifiers first, then the key, so the host sees the chord in the order a person types it */
    bool ok = wait_hid_ready() && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifiers, NULL);
    if (ok && keycode) {
        ok = wait_hid_ready() && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifiers, keys);
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_HOLD_MS));
    bool released = wait_hid_ready() && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
    ESP_LOGI(TAG, "Chord mod=0x%02X key=0x%02X %s", modifiers, keycode, ok && released ? "sent" : "not sent");
}

static void send_consumer(uint16_t usage)
{
    uint16_t none = 0;
    bool ok = wait_hid_ready() && tud_hid_report(REPORT_ID_CONSUMER, &usage, sizeof(usage));
    vTaskDelay(pdMS_TO_TICKS(KEY_HOLD_MS));
    bool released = wait_hid_ready() && tud_hid_report(REPORT_ID_CONSUMER, &none, sizeof(none));
    ESP_LOGI(TAG, "Consumer 0x%03X %s", usage, ok && released ? "sent" : "not sent");
}

static void usb_hid_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_MS));
    if (s_touch_held) {
        s_state = APP_USB_HID_STATE_JTAG;
        ESP_LOGW(TAG, "Screen held at start-up: keeping USB-Serial-JTAG until the next boot");
    } else if (install() == ESP_OK) {
        ESP_LOGI(TAG, "USB keyboard started (VID 0x%04X PID 0x%04X, serial %s)", TINYUSB_ESPRESSIF_VID, USB_PID, s_serial);
    } else {
        s_state = APP_USB_HID_STATE_JTAG;
    }

    req_t req;
    while (true) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (req.type) {
            case REQ_CHORD:
                send_chord(req.modifiers, req.keycode);
                break;
            case REQ_CONSUMER:
                send_consumer(req.usage);
                break;
            case REQ_RELEASE:
                release_to_jtag();
                break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------- */

esp_err_t app_usb_hid_init(void)
{
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    s_queue = xQueueCreate(8, sizeof(req_t));
    s_log_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_queue && s_log_lock, ESP_ERR_NO_MEM, TAG, "Out of memory");
    ESP_RETURN_ON_FALSE(xTaskCreate(usb_hid_task, "usb_hid", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create task");
    ESP_LOGI(TAG, "USB keyboard starts in %d s (hold the screen then to keep USB-Serial-JTAG)", START_DELAY_MS / 1000);
    return ESP_OK;
}

static esp_err_t enqueue(const req_t *req)
{
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    if (s_state != APP_USB_HID_STATE_ACTIVE) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, req, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_usb_hid_send_chord(uint8_t modifiers, uint8_t keycode)
{
    req_t req = { .type = REQ_CHORD, .modifiers = modifiers, .keycode = keycode };
    return enqueue(&req);
}

esp_err_t app_usb_hid_send_consumer(uint16_t usage)
{
    req_t req = { .type = REQ_CONSUMER, .usage = usage };
    return enqueue(&req);
}

esp_err_t app_usb_hid_release_to_jtag(void)
{
    req_t req = { .type = REQ_RELEASE };
    return enqueue(&req);
}

esp_err_t app_usb_hid_add_command_cb(app_usb_hid_command_cb_t cb)
{
    for (int i = 0; i < MAX_COMMAND_CBS; i++) {
        if (!s_command_cbs[i]) {
            s_command_cbs[i] = cb;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

app_usb_hid_state_t app_usb_hid_get_state(void)
{
    return s_state;
}

bool app_usb_hid_host_ready(void)
{
    return s_state == APP_USB_HID_STATE_ACTIVE && tud_mounted();
}

void app_usb_hid_on_touch(bool pressed)
{
    s_touch_held = pressed;
}

#endif /* BOARD_ENABLE_QUICK_KEYS */
