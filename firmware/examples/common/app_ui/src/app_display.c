/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <strings.h>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_log.h>

#include <esp_board_device.h>
#include <dev_display_lcd.h>
#include <dev_lcd_touch_i2c.h>

#include "app_display.h"
#include "app_dashboard.h"
#include "app_usb_hid.h"
#include "board_defs.h"

static const char *TAG = "app_display";

typedef struct {
    bool initialized;
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_io_handle_t io_handle;
    emote_handle_t emote_handle;
    esp_lcd_touch_handle_t touch_handle;
    gfx_handle_t gfx_handle;  // For polling touch events
    uint32_t h_res;
    uint32_t v_res;
} app_display_data_t;

// Forward declaration of internal emote structure to access gfx_handle
// It is part of the definition in esp_emote_espression/src/emote_init.h
struct emote_s {
    bool is_initialized;
    gfx_handle_t gfx_emote_handle;
};

static app_display_data_t s_display_data = {0};
static app_display_touch_cb_t s_touch_cb;

static const char *valid_emotions[] = {
    DISP_EMOTE_NEUTRAL,
    DISP_EMOTE_HAPPY,
    DISP_EMOTE_SAD,
    DISP_EMOTE_CRYING,
    DISP_EMOTE_ANGRY,
    DISP_EMOTE_SLEEPY,
    DISP_EMOTE_CONFUSED,
    DISP_EMOTE_SHOCKED,
    DISP_EMOTE_WINKING,
    DISP_EMOTE_IDLE,
};
static const size_t num_valid_emotions = sizeof(valid_emotions) / sizeof(valid_emotions[0]);

// Helper function to get gfx_handle from emote_handle
static gfx_handle_t emote_get_gfx_handle(emote_handle_t emote_handle)
{
    if (!emote_handle) {
        return NULL;
    }
    struct emote_s *emote = (struct emote_s *)emote_handle;
    return emote->gfx_emote_handle;
}

static void change_emotion_visibility(emote_handle_t emote_handle, bool visible)
{
    if (!emote_handle) {
        return;
    }
    gfx_obj_t *obj = emote_get_obj_by_name(emote_handle, "eye_anim");
    if (obj) {
        gfx_obj_set_visible(obj, visible);
    }
}

static void change_qrcode_visibility(emote_handle_t emote_handle, bool visible)
{
    if (!emote_handle) {
        return;
    }
    gfx_obj_t *obj = emote_get_obj_by_name(emote_handle, "qrcode");
    if (obj) {
        gfx_obj_set_visible(obj, visible);
    }
}

#ifdef BOARD_QRCODE_DISPLAY_SIZE
#include "widget/gfx_qrcode.h"

/* The emote layout only sizes the QR object's box; the QR image itself is scaled from
 * the widget's display size (100 px by default), which only gfx_qrcode_set_size() changes.
 * The widget also defaults to white modules on black, an inverted code that some scanners
 * reject, so draw standard dark modules on a white background (the spare pixels of the
 * display size become the white quiet zone).
 */
static void set_qrcode_display_size(emote_handle_t emote_handle)
{
    if (!emote_handle) {
        return;
    }
    gfx_obj_t *obj = emote_get_obj_by_name(emote_handle, "qrcode");
    if (obj) {
        gfx_qrcode_set_size(obj, BOARD_QRCODE_DISPLAY_SIZE);
        gfx_qrcode_set_color(obj, (gfx_color_t){ .full = 0x0000 });
        gfx_qrcode_set_bg_color(obj, (gfx_color_t){ .full = 0xFFFF });
    }
}
#endif

#if LEDC_BACKLIGHT_SUPPORTED

#include <dev_ledc_ctrl.h>

static bool set_lcd_backlight(int brightness_percent)
{
    periph_ledc_config_t *ledc_config = NULL;
    periph_ledc_handle_t *ledc_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_board_device_get_handle("lcd_brightness", (void **)&ledc_handle), TAG,
        "Get LEDC control device handle failed"
    );
    dev_ledc_ctrl_config_t *dev_ledc_cfg = NULL;
    ESP_RETURN_ON_ERROR(
        esp_board_device_get_config("lcd_brightness", (void **)&dev_ledc_cfg), TAG,
        "Get LEDC control device config failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_board_periph_get_config(dev_ledc_cfg->ledc_name, (void **)&ledc_config), TAG,
        "Get LEDC peripheral config failed"
    );

    uint32_t duty = (brightness_percent * ((1 << (uint32_t)ledc_config->duty_resolution) - 1)) / 100;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty), TAG, "Set LEDC duty failed"
    );
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel), TAG, "Update LEDC duty failed"
    );

    return true;
}
#endif

esp_err_t app_display_set_backlight(int percent)
{
#if LEDC_BACKLIGHT_SUPPORTED
    return set_lcd_backlight(percent) ? ESP_OK : ESP_FAIL;
#else
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void app_display_set_face_visible(bool visible)
{
    change_emotion_visibility(s_display_data.emote_handle, visible);
}

void app_display_set_touch_cb(app_display_touch_cb_t cb)
{
    s_touch_cb = cb;
}

static void touch_event_callback(gfx_handle_t handle, const gfx_touch_event_t *event, void *user_data)
{
    ESP_LOGD(TAG, "Touch event: %s, x=%u, y=%u, track_id=%u, strength=%u",
             event->type == GFX_TOUCH_EVENT_PRESS ? "PRESS" : "RELEASE",
             event->x, event->y, event->track_id, event->strength);
    bool pressed = event->type == GFX_TOUCH_EVENT_PRESS;
    app_usb_hid_on_touch(pressed);
    bool handled = app_dashboard_on_touch(pressed, event->x, event->y);
    if (s_touch_cb) {
        s_touch_cb(pressed, handled);
    }
}

esp_err_t app_display_set_text(app_device_text_type_t text_type, const char *text, void *arg)
{
    if (!s_display_data.initialized) {
        ESP_LOGE(TAG, "Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    app_dashboard_on_text(text_type, text);

    switch (text_type) {
        case APP_DEVICE_TEXT_TYPE_USER:
            // User text is not displayed through emote currently
            break;
        case APP_DEVICE_TEXT_TYPE_ASSISTANT:
            if (text && strlen(text) > 0) {
                emote_set_event_msg(s_display_data.emote_handle, EMOTE_MGR_EVT_SPEAK, text);
            }
            break;
        case APP_DEVICE_TEXT_TYPE_SYSTEM:
            emote_set_event_msg(s_display_data.emote_handle, EMOTE_MGR_EVT_SYS, text);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t app_display_system_state_changed(app_device_system_state_t new_state, void *arg)
{
    (void)arg;

    if (!s_display_data.initialized) {
        ESP_LOGE(TAG, "Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    app_dashboard_on_state(new_state);

    switch (new_state) {
        case APP_DEVICE_SYSTEM_STATE_LISTENING:
            err = app_display_set_emotion(DISP_EMOTE_IDLE);
            err = emote_set_event_msg(s_display_data.emote_handle, EMOTE_MGR_EVT_LISTEN, NULL);
            break;
        case APP_DEVICE_SYSTEM_STATE_SLEEP:
            err = app_display_set_emotion(DISP_EMOTE_SLEEPY);
            break;
        case APP_DEVICE_SYSTEM_STATE_ACTIVE:
            // These states are handled implicitly through other display updates
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    return err;
}

esp_err_t app_display_send_event(const char *event, const char *message)
{
    if (!s_display_data.initialized) {
        ESP_LOGE(TAG, "Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Send event: \"%s\", message: \"%s\"", event, message ? message : "NULL");
    return ESP_OK;
}

bool app_display_is_emotion_valid(const char *emotion)
{
    for (size_t i = 0; i < num_valid_emotions; i++) {
        if (strncasecmp(emotion, valid_emotions[i], strlen(valid_emotions[i])) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_display_set_emotion(const char *emotion)
{
    if (!s_display_data.initialized) {
        ESP_LOGE(TAG, "Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGD(TAG, "Set emotion: \"%s\"", emotion);

    for (int i = 0; i < num_valid_emotions; i++) {
        if (strncasecmp(emotion, valid_emotions[i], strlen(valid_emotions[i])) == 0) {
            emote_set_anim_emoji(s_display_data.emote_handle, valid_emotions[i]);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t app_display_show_qrcode(const char *text, const char *caption)
{
    ESP_RETURN_ON_FALSE(s_display_data.initialized && text, ESP_ERR_INVALID_STATE, TAG, "Display not ready");
    ESP_LOGI(TAG, "QR code: %s", text);
    if (caption) {
        app_display_set_text(APP_DEVICE_TEXT_TYPE_SYSTEM, caption, NULL);
    }
    change_emotion_visibility(s_display_data.emote_handle, false);
#ifdef BOARD_QRCODE_DISPLAY_SIZE
    set_qrcode_display_size(s_display_data.emote_handle);
#endif
    return emote_set_qrcode_data(s_display_data.emote_handle, text);
}

void app_display_hide_qrcode(void)
{
    if (!s_display_data.initialized) {
        return;
    }
    change_qrcode_visibility(s_display_data.emote_handle, false);
    app_display_set_emotion(DISP_EMOTE_IDLE);
}

/* Emote handle of the flush in flight. Recorded by emote_flush_callback() before each transfer
 * is queued, so the transfer-done callback never depends on app_display_init() having stored
 * the handle yet: the gfx task waits for WAIT_FLUSH_DONE with no timeout, and a lost
 * completion leaves the screen blank for the whole boot.
 */
static volatile emote_handle_t s_flush_manager = NULL;

static bool flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    emote_handle_t manager = s_flush_manager;
    if (manager) {
        emote_notify_flush_finished(manager);
    }
    return false;
}

// Flush callback for emote
static void emote_flush_callback(int x_start, int y_start, int x_end, int y_end, const void *data, emote_handle_t manager)
{
    s_flush_manager = manager;
    esp_lcd_panel_draw_bitmap(s_display_data.panel_handle, x_start, y_start, x_end, y_end, data);
}

// Initialize emote with display
static emote_handle_t init_emote(uint32_t h_res, uint32_t v_res)
{
    // default emote configuration
    emote_config_t config = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = h_res,
            .v_res = v_res,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = h_res * 10,
        },
        .task = {
            .task_priority = 5,
            .task_stack = 4096,
            .task_affinity = CONFIG_APP_EMOTE_TASK_CORE_ID,
            .task_stack_in_ext = false,
        },
        .flush_cb = emote_flush_callback,
    };

    emote_handle_t handle = emote_init(&config);
    if (!handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return NULL;
    }
    if (!emote_is_initialized(handle)) {
        ESP_LOGE(TAG, "Emote manager not initialized");
        emote_deinit(handle);
        return NULL;
    }
    return handle;
}

static esp_err_t init_display(void)
{

    ESP_LOGI(TAG, "Initializing display\n");

    dev_display_lcd_handles_t *display_handle;
    ESP_RETURN_ON_ERROR(esp_board_device_get_handle("display_lcd", (void **)&display_handle), TAG,
                        "Failed to get display handle\n");
    dev_display_lcd_config_t *display_config;
    ESP_RETURN_ON_ERROR(esp_board_device_get_config("display_lcd", (void **)&display_config), TAG,
                        "Failed to get display config\n");

    void *dev_touch_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_board_device_get_handle("lcd_touch", (void **)&dev_touch_handle), TAG,
                        "Failed to get touch handle\n");
    esp_lcd_touch_handle_t touch_handle = ((dev_lcd_touch_i2c_handles_t *)dev_touch_handle)->touch_handle;

    uint8_t *buffer = heap_caps_calloc(display_config->lcd_width * display_config->lcd_height * 2, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) {
        esp_lcd_panel_draw_bitmap(display_handle->panel_handle, 0, 0, display_config->lcd_width, display_config->lcd_height, buffer);
        heap_caps_free(buffer);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(display_handle->panel_handle, true), TAG, "Failed to turn on display\n");

#if LEDC_BACKLIGHT_SUPPORTED
    set_lcd_backlight(100);
#endif

    esp_lcd_panel_swap_xy(display_handle->panel_handle, false);

#if LCD_MIRROR_X_Y
    esp_lcd_panel_mirror(display_handle->panel_handle, true, true);
#endif

    s_display_data.panel_handle = display_handle->panel_handle;
    s_display_data.io_handle = display_handle->io_handle;
    s_display_data.touch_handle = touch_handle;
    s_display_data.h_res = display_config->lcd_width;
    s_display_data.v_res = display_config->lcd_height;


    ESP_LOGI(TAG, "Display initialized successfully");
    return ESP_OK;
}

esp_err_t app_display_init()
{
    if (s_display_data.initialized) {
        ESP_LOGW(TAG, "Display already initialized\n");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing display\n");
    ESP_RETURN_ON_ERROR(init_display(), TAG, "Failed to initialize display");

    /* Register the transfer-done callback before init_emote() starts the gfx task, so the
     * completion of its very first flush is not lost.
     */
    if (s_display_data.io_handle) {
        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = flush_io_ready_callback,
        };
        esp_err_t ret = esp_lcd_panel_io_register_event_callbacks(s_display_data.io_handle, &cbs, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register panel IO callbacks: %d", ret);
        } else {
            ESP_LOGI(TAG, "Registered panel IO transfer done callback");
        }
    }

    s_display_data.emote_handle = init_emote(s_display_data.h_res, s_display_data.v_res);

    if (s_display_data.emote_handle) {
        emote_data_t data = {
            .type = EMOTE_SOURCE_PARTITION,
            .source = {
                .partition_label = CONFIG_APP_EMOTE_PARTITION_LABEL,
            },
        };
        emote_load_assets_from_source(s_display_data.emote_handle, &data);

        emote_set_event_msg(s_display_data.emote_handle, EMOTE_MGR_EVT_SYS, "Initializing...");
        gfx_obj_t *label = emote_get_obj_by_name(s_display_data.emote_handle, "toast_label");

        /* Align the toast label to the top middle of the screen, but maintain y co-ordinate */
        gfx_coord_t x, y;
        gfx_obj_get_pos(label, &x, &y);
        gfx_obj_align(label, GFX_ALIGN_TOP_MID, 0, y);

        gfx_label_set_long_mode(label, GFX_LABEL_LONG_SCROLL);
        gfx_label_set_scroll_step(label, 4);
        gfx_label_set_scroll_speed(label, 100);
    }

    s_display_data.gfx_handle = emote_get_gfx_handle(s_display_data.emote_handle);

    if (s_display_data.touch_handle && s_display_data.gfx_handle) {
        gfx_touch_config_t touch_cfg = {
            .handle = s_display_data.touch_handle,
            .poll_ms = 15,
            .event_cb = touch_event_callback,
            .user_data = s_display_data.gfx_handle,
        };
        gfx_touch_configure(s_display_data.gfx_handle, &touch_cfg);
    }

    if (s_display_data.gfx_handle) {
        esp_err_t dash_err = app_dashboard_init(s_display_data.gfx_handle);
        if (dash_err != ESP_OK) {
            ESP_LOGW(TAG, "Dashboard init failed: %s", esp_err_to_name(dash_err));
        }
    }


    s_display_data.initialized = true;
    app_display_set_emotion(DISP_EMOTE_IDLE);

    ESP_LOGI(TAG, "Display initialized successfully");
    return ESP_OK;
}
