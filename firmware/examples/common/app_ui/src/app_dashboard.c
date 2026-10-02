/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Desk dashboard for boards that define BOARD_ENABLE_DASHBOARD.
 *
 * Pages (swipe left/right): clock + weather, the quick-key pages (app_quick_keys.c, when the board
 * enables them), Neurons (app_neurons.c: tap it to play the name and texts), and for a voice device
 * (app_dashboard_configure) timers and the conversation. The dashboard opens on Neurons (PAGE_START).
 * A rate ticker runs along the bottom of the clock and timer pages, with the battery level under it on
 * the clock page (app_battery.c). While Wi-Fi setup runs, the clock page shows a QR code for the setup
 * network instead, and the dashboard opens there.
 * Everything is drawn with esp_emote_gfx objects created after the emote objects, so a full-screen
 * backdrop hides the emote face while a page is shown.
 *
 * Threading: other tasks only write s_shared (under its mutex); ui_tick() runs as a gfx timer in the
 * gfx task and is the only place that touches the gfx objects after init.
 */

#include "app_dashboard.h"

#if BOARD_ENABLE_DASHBOARD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>
#include <esp_check.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "gfx.h"
#include "app_battery.h"
#include "app_quick_keys.h"
#include "app_display.h"
#include "app_neurons.h"
#include "app_settings.h"
#include "app_usb_hid.h"

static const char *TAG = "app_dashboard";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_48);

#define FONT(f) ((gfx_font_t)&(f))

#define COLOR_BG            0x000000
#define COLOR_TEXT          0xFFFFFF
#define COLOR_DIM           0x8A94A6
#define COLOR_ACCENT        0x5EC8F2
#define COLOR_TICKER        0xFFC857
#define COLOR_USER          0xA8B3C4
#define COLOR_LOW           0xFFC857    /* battery at BATTERY_LOW_PCT and below */
#define COLOR_CRITICAL      0xFF6B5E    /* battery at BATTERY_CRITICAL_PCT and below */

#define DEVICE_NAME         "VoCat"
#define SWIPE_MIN_PX        60
#define VOLUME_STEP         5
#define VOLUME_ROW_Y_MIN    235     /* screen band of the volume row on the timers page */
#define VOLUME_ROW_Y_MAX    300
#define VOLUME_TAP_LEFT_X   150     /* tap left of this: quieter; right of VOLUME_TAP_RIGHT_X: louder */
#define VOLUME_TAP_RIGHT_X  210
#define UI_TICK_MS          250
#define SETUP_QR_SIZE       130     /* Wi-Fi setup QR code, px */
#define GEO_PERIOD_S        (6 * 60 * 60)
#define WEATHER_PERIOD_S    (15 * 60)
#define RATES_PERIOD_S      (5 * 60)
#define RETRY_PERIOD_S      60
#define HTTP_BUF_SIZE       (16 * 1024)
#define MAX_TIMERS          4
#define TIMER_DONE_SHOW_US  (10LL * 1000 * 1000)
#define SLEEP_RETURN_US     (3LL * 1000 * 1000)
#define NETWORK_READY_BIT   BIT0

#define BATTERY_LOW_PCT     20
#define BATTERY_CRITICAL_PCT 10

#define DEG                 "\xC2\xB0"      /* ° */
#define BULLET              "\xE2\x80\xA2"  /* • */

/* FontAwesome symbols in the Montserrat fonts (LV_SYMBOL_BATTERY_*), empty to full, and LV_SYMBOL_CHARGE */
static const char *const BATTERY_GLYPHS[] = {
    "\xEF\x89\x84", "\xEF\x89\x83", "\xEF\x89\x82", "\xEF\x89\x81", "\xEF\x89\x80",
};
#define CHARGE_GLYPH        "\xEF\x83\xA7"

typedef enum {
    PAGE_CLOCK = 0,
#if BOARD_ENABLE_QUICK_KEYS
    PAGE_KEYS_FIRST,
    PAGE_KEYS_LAST = PAGE_KEYS_FIRST + APP_QUICK_KEYS_PAGE_COUNT - 1,
#endif
    PAGE_NEURONS,
    PAGE_TIMERS,
    PAGE_CONVERSATION,
    PAGE_COUNT,
} dash_page_t;

#define PAGE_START          PAGE_NEURONS    /* shown when the dashboard appears, and after Wi-Fi setup */

typedef struct {
    char label[40];
    int64_t end_us;     /* 0 = free slot */
} dash_timer_t;

/* Written by other tasks under s_shared.lock, read by ui_tick() */
static struct {
    SemaphoreHandle_t lock;
    bool network_ready;
    char weather[64];
    char weather_sub[96];
    char ticker[320];
    char user_text[256];
    char assistant_text[640];
    char status_text[96];
    dash_timer_t timers[MAX_TIMERS];
    int requested_page;         /* -1 = none */
    int64_t return_at_us;       /* leave the conversation view at this time, 0 = not pending */
    struct {
        bool pending, pressed;
        uint16_t x, y;
    } fake_touch;               /* from "vocat touch", played in ui_tick */
    char notice[48];            /* a line at the bottom of the clock page, "" = none */
    int64_t notice_until_us;
    bool notice_dirty;
    char setup_ssid[33];        /* Wi-Fi setup network shown on the clock page, "" = none */
    char setup_address[40];     /* where the setup page is, once joined */
    bool setup_dirty;
    bool text_dirty;
    bool data_dirty;
} s_shared = { .requested_page = -1 };

/* Set once by app_dashboard_configure() right after init */
static app_dashboard_config_t s_cfg = { .voice = true };

/* Owned by the gfx task after init */
static struct {
    gfx_handle_t gfx;
    dash_page_t page;
    dash_page_t home_page;      /* last page shown other than the conversation, returned to after it */
    bool visible;
    gfx_obj_t *backdrop;
    gfx_obj_t *time_label, *date_label, *weather_label, *weather_sub_label;
    gfx_obj_t *timers_title, *timers_body, *volume_label, *vol_down_label, *vol_up_label;
    gfx_obj_t *convo_status, *convo_user, *convo_assistant;
    gfx_obj_t *ticker_label;
    gfx_obj_t *setup_title, *setup_qr, *setup_hint, *setup_ssid, *setup_address;
    gfx_obj_t *name_label, *notice_label, *battery_label;
    uint32_t shown_settings;
    uint32_t shown_battery;     /* app_battery_state_t.version on screen */
    bool battery_valid;
    char shown_name[APP_SETTINGS_NAME_MAX + 1];
    bool setup_shown;           /* the clock page shows Wi-Fi setup instead of the clock */
    char shown_time[24];
    char shown_date[48];
    char shown_timers[256];
    int shown_volume;
    uint16_t touch_x0, touch_y0;
    bool touch_down;
} s_ui = { .shown_volume = -1 };

/* Data task state */
static struct {
    EventGroupHandle_t events;
    char *buf;
    bool geo_ok;
    double lat, lon;
    char city[48];
    double usd_hkd;
    int64_t next_geo_us, next_weather_us, next_rates_us;
} s_data;

/* Volume steps from the screen are queued here and applied by volume_task: the touch callback runs
 * in the gfx task, and the NVS write plus cloud update behind a volume change are too heavy for it.
 */
static QueueHandle_t s_volume_queue;

/* ------------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------------- */

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!src) {
        src = "";
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* Format v with the given decimals and thousands separators, e.g. 617700 -> "617,700" */
static void fmt_number(char *out, size_t size, double v, int decimals)
{
    char raw[40];
    snprintf(raw, sizeof(raw), "%.*f", decimals, v);

    const char *digits = raw[0] == '-' ? raw + 1 : raw;
    const char *dot = strchr(digits, '.');
    int int_len = dot ? (int)(dot - digits) : (int)strlen(digits);

    char tmp[48];
    size_t pos = 0;
    if (raw[0] == '-') {
        tmp[pos++] = '-';
    }
    for (int i = 0; i < int_len && pos < sizeof(tmp) - 1; i++) {
        tmp[pos++] = digits[i];
        int remaining = int_len - i - 1;
        if (remaining > 0 && remaining % 3 == 0 && pos < sizeof(tmp) - 1) {
            tmp[pos++] = ',';
        }
    }
    if (dot) {
        for (const char *p = dot; *p && pos < sizeof(tmp) - 1; p++) {
            tmp[pos++] = *p;
        }
    }
    tmp[pos] = '\0';
    copy_str(out, size, tmp);
}

static const char *weather_code_text(int code)
{
    if (code == 0) return "Clear";
    if (code == 1) return "Mostly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "--";
}

static gfx_obj_t *make_label(const lv_font_t *font, uint32_t color, uint16_t w, uint16_t h,
                             uint8_t align, gfx_coord_t x, gfx_coord_t y, gfx_label_long_mode_t mode)
{
    gfx_obj_t *label = gfx_label_create(s_ui.gfx);
    if (!label) {
        return NULL;
    }
    gfx_label_set_font(label, (gfx_font_t)font);
    gfx_label_set_color(label, GFX_COLOR_HEX(color));
    gfx_label_set_text_align(label, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(label, mode);
    gfx_obj_set_size(label, w, h);
    gfx_obj_align(label, align, x, y);
    gfx_label_set_text(label, "");
    gfx_obj_set_visible(label, false);
    return label;
}

static void set_label_text(gfx_obj_t *label, const char *text)
{
    if (label) {
        gfx_label_set_text(label, (text && text[0]) ? text : " ");
    }
}

static void set_visible(gfx_obj_t *obj, bool visible)
{
    if (obj) {
        gfx_obj_set_visible(obj, visible);
    }
}

/* ------------------------------------------------------------------------------------------------
 * UI (gfx task)
 * ---------------------------------------------------------------------------------------------- */

/* Index of the quick-key page on dashboard page `page`, or -1 */
static int key_page(dash_page_t page)
{
#if BOARD_ENABLE_QUICK_KEYS
    if (page >= PAGE_KEYS_FIRST && page <= PAGE_KEYS_LAST) {
        return page - PAGE_KEYS_FIRST;
    }
#endif
    (void)page;
    return -1;
}

/* Pages the swipe cycle and "vocat page" can reach */
static bool page_enabled(int page)
{
    return page >= 0 && page < PAGE_COUNT && (s_cfg.voice || (page != PAGE_TIMERS && page != PAGE_CONVERSATION));
}

static int next_page(int page, int dir)
{
    do {
        page = (page + dir + PAGE_COUNT) % PAGE_COUNT;
    } while (!page_enabled(page));
    return page;
}

static void show_page(dash_page_t page)
{
    s_ui.page = page;
    if (page != PAGE_CONVERSATION) {
        s_ui.home_page = page;
    }
    bool v = s_ui.visible;

    /* The Neurons page is an opaque full-screen image: the backdrop under it would only cost time */
    bool neurons = app_neurons_show(v && page == PAGE_NEURONS);
    set_visible(s_ui.backdrop, v && !neurons);

    bool clock = v && page == PAGE_CLOCK && !s_ui.setup_shown;
    bool setup = v && page == PAGE_CLOCK && s_ui.setup_shown;
    set_visible(s_ui.name_label, clock && s_ui.shown_name[0]);
    set_visible(s_ui.notice_label, clock && s_shared.notice[0]);
    set_visible(s_ui.time_label, clock);
    set_visible(s_ui.date_label, clock);
    set_visible(s_ui.weather_label, clock);
    set_visible(s_ui.weather_sub_label, clock);
    set_visible(s_ui.battery_label, clock && s_ui.battery_valid);

    set_visible(s_ui.setup_title, setup);
    set_visible(s_ui.setup_qr, setup);
    set_visible(s_ui.setup_hint, setup);
    set_visible(s_ui.setup_ssid, setup);
    set_visible(s_ui.setup_address, setup);

    set_visible(s_ui.timers_title, v && page == PAGE_TIMERS);
    set_visible(s_ui.timers_body, v && page == PAGE_TIMERS);
    bool volume = v && page == PAGE_TIMERS && s_cfg.get_volume && s_cfg.set_volume;
    set_visible(s_ui.volume_label, volume);
    set_visible(s_ui.vol_down_label, volume);
    set_visible(s_ui.vol_up_label, volume);

    set_visible(s_ui.convo_status, v && page == PAGE_CONVERSATION);
    set_visible(s_ui.convo_user, v && page == PAGE_CONVERSATION);
    set_visible(s_ui.convo_assistant, v && page == PAGE_CONVERSATION);

    set_visible(s_ui.ticker_label, (clock && !s_shared.notice[0]) || (v && page == PAGE_TIMERS));

#if BOARD_ENABLE_QUICK_KEYS
    app_quick_keys_show(v ? key_page(page) : -1);
#endif
}

static bool any_timer_active(const dash_timer_t *timers)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].end_us) {
            return true;
        }
    }
    return false;
}

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char time_str[24];
    char date_str[48];
    if (tm.tm_year + 1900 < 2024) {
        copy_str(time_str, sizeof(time_str), "--:--");
        copy_str(date_str, sizeof(date_str), "Waiting for time...");
    } else {
        int hour12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
        snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        strftime(date_str, sizeof(date_str), "%A, %d %b", &tm);
    }

    if (strcmp(time_str, s_ui.shown_time) != 0) {
        copy_str(s_ui.shown_time, sizeof(s_ui.shown_time), time_str);
        set_label_text(s_ui.time_label, time_str);
    }
    if (strcmp(date_str, s_ui.shown_date) != 0) {
        copy_str(s_ui.shown_date, sizeof(s_ui.shown_date), date_str);
        set_label_text(s_ui.date_label, date_str);
    }
}

/* Called with s_shared.lock held */
static void update_timers_locked(void)
{
    char body[256] = "";
    size_t len = 0;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < MAX_TIMERS; i++) {
        dash_timer_t *t = &s_shared.timers[i];
        if (!t->end_us) {
            continue;
        }
        int64_t left_us = t->end_us - now;
        if (left_us <= -TIMER_DONE_SHOW_US) {
            t->end_us = 0;
            continue;
        }
        char line[80];
        if (left_us <= 0) {
            snprintf(line, sizeof(line), "%s  Done!\n", t->label);
        } else {
            int left = (int)((left_us + 999999) / 1000000);
            if (left >= 3600) {
                snprintf(line, sizeof(line), "%s  %d:%02d:%02d\n", t->label, left / 3600, (left / 60) % 60, left % 60);
            } else {
                snprintf(line, sizeof(line), "%s  %d:%02d\n", t->label, left / 60, left % 60);
            }
        }
        len += snprintf(body + len, sizeof(body) - len, "%s", line);
        if (len >= sizeof(body)) {
            break;
        }
    }
    if (body[0] == '\0') {
        copy_str(body, sizeof(body), "No timers running\n\nTry: \"remind me in 5 minutes\"");
    }

    if (strcmp(body, s_ui.shown_timers) != 0) {
        copy_str(s_ui.shown_timers, sizeof(s_ui.shown_timers), body);
        set_label_text(s_ui.timers_body, body);
    }
}

static void update_volume(void)
{
    if (!s_cfg.get_volume) {
        return;
    }
    int volume = s_cfg.get_volume();
    if (volume != s_ui.shown_volume) {
        s_ui.shown_volume = volume;
        char text[32];
        snprintf(text, sizeof(text), "Volume %d%%", volume);
        set_label_text(s_ui.volume_label, text);
    }
}

/* Glyph and percentage under the ticker, coloured only when it needs attention */
static void update_battery(void)
{
    app_battery_state_t b;
    app_battery_get(&b);
    if (b.version == s_ui.shown_battery) {
        return;
    }
    s_ui.shown_battery = b.version;

    if (b.valid) {
        char text[40];
        const char *glyph = BATTERY_GLYPHS[(b.percent + 12) / 25];
        uint32_t color = COLOR_DIM;
        if (b.on_usb) {
            color = COLOR_ACCENT;
            if (b.percent >= 100) {
                snprintf(text, sizeof(text), "%s  Charged", glyph);
            } else {
                snprintf(text, sizeof(text), CHARGE_GLYPH " %s  %d%%", glyph, b.percent);
            }
        } else if (b.percent <= BATTERY_CRITICAL_PCT) {
            color = COLOR_CRITICAL;
            snprintf(text, sizeof(text), "%s  %d%% " BULLET " Charge me", glyph, b.percent);
        } else {
            color = b.percent <= BATTERY_LOW_PCT ? COLOR_LOW : COLOR_DIM;
            snprintf(text, sizeof(text), "%s  %d%%", glyph, b.percent);
        }
        gfx_label_set_color(s_ui.battery_label, GFX_COLOR_HEX(color));
        set_label_text(s_ui.battery_label, text);
    }
    if (b.valid != s_ui.battery_valid) {
        s_ui.battery_valid = b.valid;
        if (s_ui.visible) {
            show_page(s_ui.page);
        }
    }
}

static void ui_tick(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_shared.lock, 0) != pdTRUE) {
        return;     /* try again next tick rather than blocking the gfx task */
    }

    if ((s_shared.network_ready || !s_cfg.voice) && !s_ui.visible) {
        s_ui.visible = true;
        show_page(s_ui.setup_shown ? PAGE_CLOCK : PAGE_START);
    }

    if (s_shared.requested_page >= 0) {
        if (s_ui.visible) {
            show_page((dash_page_t)s_shared.requested_page);
        }
        s_shared.requested_page = -1;
    }

    if (s_shared.return_at_us && esp_timer_get_time() >= s_shared.return_at_us) {
        s_shared.return_at_us = 0;
        if (s_ui.visible && s_ui.page == PAGE_CONVERSATION) {
            show_page(any_timer_active(s_shared.timers) ? PAGE_TIMERS : s_ui.home_page);
        }
    }

    if (s_shared.text_dirty) {
        s_shared.text_dirty = false;
        set_label_text(s_ui.convo_status, s_shared.status_text);
        set_label_text(s_ui.convo_user, s_shared.user_text);
        set_label_text(s_ui.convo_assistant, s_shared.assistant_text);
    }

    if (s_ui.shown_settings != app_settings_version()) {
        s_ui.shown_settings = app_settings_version();
        char name[APP_SETTINGS_NAME_MAX + 1];
        app_settings_get_name(name, sizeof(name));
        if (strcmp(name, s_ui.shown_name) != 0) {
            copy_str(s_ui.shown_name, sizeof(s_ui.shown_name), name);
            set_label_text(s_ui.name_label, name);
            if (s_ui.visible) {
                show_page(s_ui.page);
            }
        }
        app_display_set_backlight(app_settings_get_brightness());
    }

    if (s_shared.notice_dirty ||
            (s_shared.notice[0] && s_shared.notice_until_us && esp_timer_get_time() > s_shared.notice_until_us)) {
        s_shared.notice_dirty = false;
        if (s_shared.notice_until_us && esp_timer_get_time() > s_shared.notice_until_us) {
            s_shared.notice[0] = '\0';
        }
        set_label_text(s_ui.notice_label, s_shared.notice);
        if (s_ui.visible) {
            show_page(s_ui.page);
        }
    }

    if (s_shared.setup_dirty) {
        s_shared.setup_dirty = false;
        s_ui.setup_shown = s_shared.setup_ssid[0] != '\0';
        if (s_ui.setup_shown && s_ui.setup_qr) {
            /* Joins an open network when scanned with a phone camera */
            char qr[80];
            snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", s_shared.setup_ssid);
            gfx_qrcode_set_data(s_ui.setup_qr, qr);
            set_label_text(s_ui.setup_ssid, s_shared.setup_ssid);
            char open[56];
            snprintf(open, sizeof(open), "Open %s", s_shared.setup_address);
            set_label_text(s_ui.setup_address, open);
        }
        if (s_ui.visible) {
            show_page(s_ui.setup_shown ? PAGE_CLOCK : PAGE_START);
        }
    }

    if (s_shared.data_dirty) {
        s_shared.data_dirty = false;
        set_label_text(s_ui.weather_label, s_shared.weather);
        set_label_text(s_ui.weather_sub_label, s_shared.weather_sub);
        set_label_text(s_ui.ticker_label, s_shared.ticker);
    }

    update_timers_locked();
    bool fake_touch = s_shared.fake_touch.pending;
    bool fake_pressed = s_shared.fake_touch.pressed;
    uint16_t fake_x = s_shared.fake_touch.x, fake_y = s_shared.fake_touch.y;
    s_shared.fake_touch.pending = false;
    xSemaphoreGive(s_shared.lock);

    if (fake_touch) {
        app_dashboard_on_touch(fake_pressed, fake_x, fake_y);     /* takes the lock itself on a swipe */
    }
    update_clock();
    update_battery();
    update_volume();
#if BOARD_ENABLE_QUICK_KEYS
    app_quick_keys_tick();
#endif
}

/* ------------------------------------------------------------------------------------------------
 * Data (network task)
 * ---------------------------------------------------------------------------------------------- */

static int http_get(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .user_agent = "Mozilla/5.0 (ESP32-S3; desk dashboard)",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }

    int total = -1;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        total = 0;
        while (total < HTTP_BUF_SIZE - 1) {
            int n = esp_http_client_read(client, s_data.buf + total, HTTP_BUF_SIZE - 1 - total);
            if (n <= 0) {
                break;
            }
            total += n;
        }
        s_data.buf[total] = '\0';
        if (status != 200) {
            ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
            total = -1;
        }
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total;
}

static cJSON *get_json(const char *url)
{
    if (http_get(url) <= 0) {
        return NULL;
    }
    cJSON *json = cJSON_Parse(s_data.buf);
    if (!json) {
        ESP_LOGW(TAG, "Invalid JSON from %s", url);
    }
    return json;
}

static double json_number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool fetch_geo(void)
{
    cJSON *json = get_json("http://ip-api.com/json/?fields=status,city,lat,lon,offset");
    if (!json) {
        return false;
    }
    bool ok = false;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        s_data.lat = json_number(json, "lat", 0);
        s_data.lon = json_number(json, "lon", 0);
        const cJSON *city = cJSON_GetObjectItemCaseSensitive(json, "city");
        copy_str(s_data.city, sizeof(s_data.city), cJSON_IsString(city) ? city->valuestring : "");

        /* POSIX TZ signs are inverted: UTC+7 is "UTC-7:00" */
        int offset = (int)json_number(json, "offset", 0);
        int abs_off = abs(offset);
        char tz[24];
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d", offset >= 0 ? '-' : '+', abs_off / 3600, (abs_off % 3600) / 60);
        setenv("TZ", tz, 1);
        tzset();

        ESP_LOGI(TAG, "Location: %s (%.3f, %.3f), TZ %s", s_data.city, s_data.lat, s_data.lon, tz);
        ok = true;
    }
    cJSON_Delete(json);
    return ok;
}

static bool fetch_weather(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1", s_data.lat, s_data.lon);
    cJSON *json = get_json(url);
    if (!json) {
        return false;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    bool ok = cJSON_IsObject(current);
    if (ok) {
        double temp = json_number(current, "temperature_2m", 0);
        int code = (int)json_number(current, "weather_code", -1);
        double hi = 0, lo = 0;
        const cJSON *max = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max") : NULL;
        const cJSON *min = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min") : NULL;
        if (cJSON_IsArray(max) && cJSON_IsArray(min) && cJSON_GetArraySize(max) > 0 && cJSON_GetArraySize(min) > 0) {
            hi = cJSON_GetArrayItem(max, 0)->valuedouble;
            lo = cJSON_GetArrayItem(min, 0)->valuedouble;
        }

        xSemaphoreTake(s_shared.lock, portMAX_DELAY);
        snprintf(s_shared.weather, sizeof(s_shared.weather), "%.0f" DEG "C  %s", temp, weather_code_text(code));
        snprintf(s_shared.weather_sub, sizeof(s_shared.weather_sub), "H %.0f" DEG "  L %.0f" DEG "  " BULLET "  %s",
                 hi, lo, s_data.city);
        s_shared.data_dirty = true;
        xSemaphoreGive(s_shared.lock);
    }
    cJSON_Delete(json);
    return ok;
}

static bool fetch_rates(void)
{
    char parts[4][96] = { "", "", "", "" };

    /* USD -> HKD exchange rate */
    cJSON *json = get_json("https://open.er-api.com/v6/latest/USD");
    if (json) {
        const cJSON *rates = cJSON_GetObjectItemCaseSensitive(json, "rates");
        double hkd = rates ? json_number(rates, "HKD", 0) : 0;
        if (hkd > 0) {
            s_data.usd_hkd = hkd;
        }
        cJSON_Delete(json);
    }
    if (s_data.usd_hkd > 0) {
        snprintf(parts[0], sizeof(parts[0]), "USD/HKD %.3f", s_data.usd_hkd);
    }

    /* BTC in USD with 24 h change */
    json = get_json("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true");
    if (json) {
        const cJSON *btc = cJSON_GetObjectItemCaseSensitive(json, "bitcoin");
        double price = btc ? json_number(btc, "usd", 0) : 0;
        double change = btc ? json_number(btc, "usd_24h_change", 0) : 0;
        if (price > 0) {
            char num[32];
            fmt_number(num, sizeof(num), price, 0);
            snprintf(parts[1], sizeof(parts[1]), "BTC $%s %+.1f%%", num, change);
        }
        cJSON_Delete(json);
    }

    /* Gold in USD per troy ounce */
    json = get_json("https://api.gold-api.com/price/XAU");
    if (json) {
        double usd = json_number(json, "price", 0);
        if (usd > 0) {
            char num[32];
            fmt_number(num, sizeof(num), usd, 0);
            snprintf(parts[2], sizeof(parts[2]), "GOLD $%s/oz", num);
        }
        cJSON_Delete(json);
    }

    /* S&P 500 index level (points, USD-denominated) with change vs previous close */
    json = get_json("https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?range=1d&interval=1d");
    if (json) {
        const cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
        const cJSON *result = chart ? cJSON_GetObjectItemCaseSensitive(chart, "result") : NULL;
        const cJSON *first = cJSON_IsArray(result) ? cJSON_GetArrayItem(result, 0) : NULL;
        const cJSON *meta = first ? cJSON_GetObjectItemCaseSensitive(first, "meta") : NULL;
        double price = meta ? json_number(meta, "regularMarketPrice", 0) : 0;
        double prev = meta ? json_number(meta, "chartPreviousClose", 0) : 0;
        if (price > 0) {
            char num[32];
            fmt_number(num, sizeof(num), price, 0);
            if (prev > 0) {
                snprintf(parts[3], sizeof(parts[3]), "S&P 500 %s %+.1f%%", num, (price - prev) / prev * 100.0);
            } else {
                snprintf(parts[3], sizeof(parts[3]), "S&P 500 %s", num);
            }
        }
        cJSON_Delete(json);
    }

    char ticker[320] = "";
    size_t len = 0;
    for (int i = 0; i < 4; i++) {
        if (parts[i][0] == '\0') {
            continue;
        }
        len += snprintf(ticker + len, sizeof(ticker) - len, "%s%s", len ? "   " BULLET "   " : "", parts[i]);
        if (len >= sizeof(ticker)) {
            break;
        }
    }
    if (ticker[0] == '\0') {
        return false;
    }

    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    copy_str(s_shared.ticker, sizeof(s_shared.ticker), ticker);
    s_shared.data_dirty = true;
    xSemaphoreGive(s_shared.lock);
    ESP_LOGI(TAG, "Ticker: %s", ticker);
    return true;
}

static void data_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_data.events, NETWORK_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (true) {
        int64_t now = esp_timer_get_time();

        if (now >= s_data.next_geo_us) {
            s_data.geo_ok = fetch_geo() || s_data.geo_ok;
            s_data.next_geo_us = now + (int64_t)(s_data.geo_ok ? GEO_PERIOD_S : RETRY_PERIOD_S) * 1000000;
        }
        if (s_data.geo_ok && now >= s_data.next_weather_us) {
            bool ok = fetch_weather();
            s_data.next_weather_us = now + (int64_t)(ok ? WEATHER_PERIOD_S : RETRY_PERIOD_S) * 1000000;
        }
        if (now >= s_data.next_rates_us) {
            bool ok = fetch_rates();
            s_data.next_rates_us = now + (int64_t)(ok ? RATES_PERIOD_S : RETRY_PERIOD_S) * 1000000;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    s_shared.network_ready = true;
    xSemaphoreGive(s_shared.lock);
    xEventGroupSetBits(s_data.events, NETWORK_READY_BIT);
}

#if BOARD_ENABLE_QUICK_KEYS
/* Development commands on the USB control port, for checking the screen without touching it (e.g. with a
 * camera). Runs in the TinyUSB task.
 *   vocat page <n>              show dashboard page n (0 = clock, then the key pages, Neurons, timers,
 *                               conversation)
 *   vocat touch <0|1> <x> <y>   release or press at screen x, y, played through the dashboard touch path in
 *                               ui_tick (the agent-interrupt and factory-reset handling is skipped)
 * and for setting up the board without its settings page (the setters only touch memory):
 *   vocat set name <text>       the name on the clock page, also the first text on the Neurons page
 *   vocat set text <1-4> [text] a Neurons text after the name; no text clears it
 *   vocat set color <RRGGBB>    the Neurons page colour
 *   vocat set hold <seconds>    how long the Neurons page holds each text
 */
static void usb_command(const char *line)
{
    int page, pressed, x, y, n;
    unsigned int rgb;
    char hex[8];
    if (strncmp(line, "vocat set name ", 15) == 0) {
        app_settings_set_name(line + 15);
    } else if (sscanf(line, "vocat set text %d%n", &page, &n) == 1 && page >= 1 && page <= APP_SETTINGS_TEXT_COUNT) {
        app_settings_set_text(page - 1, line[n] == ' ' ? line + n + 1 : "");
    } else if (sscanf(line, "vocat set color %7s", hex) == 1 && strlen(hex) == 6 && sscanf(hex, "%x", &rgb) == 1) {
        app_settings_set_neuron_color(rgb);
    } else if (sscanf(line, "vocat set hold %d", &n) == 1) {
        app_settings_set_neuron_hold(n);
    } else if (sscanf(line, "vocat page %d", &page) == 1 && page_enabled(page)) {
        xSemaphoreTake(s_shared.lock, portMAX_DELAY);
        s_shared.requested_page = page;
        s_shared.return_at_us = 0;
        xSemaphoreGive(s_shared.lock);
    } else if (sscanf(line, "vocat touch %d %d %d", &pressed, &x, &y) == 3 && x >= 0 && x < 360 && y >= 0 && y < 360) {
        xSemaphoreTake(s_shared.lock, portMAX_DELAY);
        s_shared.fake_touch.pending = true;
        s_shared.fake_touch.pressed = pressed != 0;
        s_shared.fake_touch.x = (uint16_t)x;
        s_shared.fake_touch.y = (uint16_t)y;
        xSemaphoreGive(s_shared.lock);
    }
}
#endif

static void volume_task(void *arg)
{
    (void)arg;
    while (true) {
        int8_t step = 0;
        if (xQueueReceive(s_volume_queue, &step, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int delta = step;
        while (xQueueReceive(s_volume_queue, &step, pdMS_TO_TICKS(150)) == pdTRUE) {
            delta += step;      /* coalesce quick repeated taps into a single write */
        }

        if (!s_cfg.get_volume || !s_cfg.set_volume) {
            continue;
        }
        int volume = (int)s_cfg.get_volume() + delta;
        volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
        esp_err_t err = s_cfg.set_volume((uint8_t)volume);
        ESP_LOGI(TAG, "Volume from the screen: %d (%s)", volume, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------- */

esp_err_t app_dashboard_init(void *gfx_handle)
{
    ESP_RETURN_ON_FALSE(gfx_handle, ESP_ERR_INVALID_ARG, TAG, "No gfx handle");
    ESP_RETURN_ON_FALSE(s_ui.gfx == NULL, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    s_shared.lock = xSemaphoreCreateMutex();
    s_data.events = xEventGroupCreate();
    s_data.buf = heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_shared.lock && s_data.events && s_data.buf, ESP_ERR_NO_MEM, TAG, "Out of memory");

    s_ui.gfx = (gfx_handle_t)gfx_handle;
    copy_str(s_shared.status_text, sizeof(s_shared.status_text), "Say \"Hi, ESP\"");
    copy_str(s_shared.weather, sizeof(s_shared.weather), "Loading weather...");
    copy_str(s_shared.ticker, sizeof(s_shared.ticker), "Loading rates...");
    s_shared.text_dirty = true;
    s_shared.data_dirty = true;

    gfx_emote_lock(s_ui.gfx);

    /* Full-screen backdrop that hides the emote UI while a dashboard page is shown */
    s_ui.backdrop = make_label(&lv_font_montserrat_14, COLOR_BG, 360, 360, GFX_ALIGN_CENTER, 0, 0, GFX_LABEL_LONG_CLIP);
    if (s_ui.backdrop) {
        gfx_label_set_bg_color(s_ui.backdrop, GFX_COLOR_HEX(COLOR_BG));
        gfx_label_set_bg_enable(s_ui.backdrop, true);
        gfx_label_set_text(s_ui.backdrop, " ");
    }

    /* Clock + weather */
    s_ui.date_label = make_label(&lv_font_montserrat_20, COLOR_DIM, 260, 28, GFX_ALIGN_TOP_MID, 0, 72, GFX_LABEL_LONG_CLIP);
    s_ui.time_label = make_label(&lv_font_montserrat_48, COLOR_TEXT, 300, 60, GFX_ALIGN_CENTER, 0, -45, GFX_LABEL_LONG_CLIP);
    s_ui.weather_label = make_label(&lv_font_montserrat_28, COLOR_ACCENT, 300, 36, GFX_ALIGN_CENTER, 0, 20, GFX_LABEL_LONG_CLIP);
    s_ui.weather_sub_label = make_label(&lv_font_montserrat_14, COLOR_DIM, 280, 22, GFX_ALIGN_CENTER, 0, 55, GFX_LABEL_LONG_CLIP);

    /* Timers + volume */
    s_ui.timers_title = make_label(&lv_font_montserrat_20, COLOR_ACCENT, 200, 28, GFX_ALIGN_TOP_MID, 0, 62, GFX_LABEL_LONG_CLIP);
    s_ui.timers_body = make_label(&lv_font_montserrat_20, COLOR_TEXT, 270, 150, GFX_ALIGN_CENTER, 0, -5, GFX_LABEL_LONG_WRAP);
    s_ui.volume_label = make_label(&lv_font_montserrat_14, COLOR_DIM, 200, 22, GFX_ALIGN_CENTER, 0, 82, GFX_LABEL_LONG_CLIP);
    s_ui.vol_down_label = make_label(&lv_font_montserrat_28, COLOR_ACCENT, 60, 36, GFX_ALIGN_CENTER, -115, 78, GFX_LABEL_LONG_CLIP);
    s_ui.vol_up_label = make_label(&lv_font_montserrat_28, COLOR_ACCENT, 60, 36, GFX_ALIGN_CENTER, 115, 78, GFX_LABEL_LONG_CLIP);
    set_label_text(s_ui.timers_title, "Timers");
    set_label_text(s_ui.vol_down_label, "-");
    set_label_text(s_ui.vol_up_label, "+");

    /* Conversation */
    s_ui.convo_status = make_label(&lv_font_montserrat_20, COLOR_ACCENT, 240, 28, GFX_ALIGN_TOP_MID, 0, 55, GFX_LABEL_LONG_CLIP);
    s_ui.convo_user = make_label(&lv_font_montserrat_20, COLOR_USER, 270, 70, GFX_ALIGN_TOP_MID, 0, 92, GFX_LABEL_LONG_WRAP);
    s_ui.convo_assistant = make_label(&lv_font_montserrat_20, COLOR_TEXT, 290, 140, GFX_ALIGN_TOP_MID, 0, 168, GFX_LABEL_LONG_WRAP);

    /* Rate ticker along the bottom */
    s_ui.ticker_label = make_label(&lv_font_montserrat_20, COLOR_TICKER, 250, 28, GFX_ALIGN_BOTTOM_MID, 0, -62, GFX_LABEL_LONG_SCROLL);
    if (s_ui.ticker_label) {
        gfx_label_set_scroll_loop(s_ui.ticker_label, true);
        gfx_label_set_scroll_step(s_ui.ticker_label, 2);
        gfx_label_set_scroll_speed(s_ui.ticker_label, 40);
    }

    /* Name above the date, and a notice line in the ticker's place */
    s_ui.name_label = make_label(&lv_font_montserrat_14, COLOR_ACCENT, 240, 20, GFX_ALIGN_TOP_MID, 0, 46, GFX_LABEL_LONG_CLIP);
    s_ui.notice_label = make_label(&lv_font_montserrat_20, COLOR_ACCENT, 280, 28, GFX_ALIGN_BOTTOM_MID, 0, -62, GFX_LABEL_LONG_CLIP);

    /* Battery level under the ticker: the screen is about 180 px wide there */
    s_ui.battery_label = make_label(&lv_font_montserrat_14, COLOR_DIM, 170, 18, GFX_ALIGN_BOTTOM_MID, 0, -30, GFX_LABEL_LONG_CLIP);

    /* Neurons page, full screen over the backdrop */
    esp_err_t neurons_err = app_neurons_create(s_ui.gfx);
    if (neurons_err != ESP_OK) {
        ESP_LOGW(TAG, "Neurons page unavailable: %s", esp_err_to_name(neurons_err));
    }

    /* Wi-Fi setup view on the clock page (app_dashboard_set_wifi_setup) */
    s_ui.setup_title = make_label(&lv_font_montserrat_20, COLOR_ACCENT, 240, 28, GFX_ALIGN_TOP_MID, 0, 40, GFX_LABEL_LONG_CLIP);
    set_label_text(s_ui.setup_title, "Wi-Fi setup");
    s_ui.setup_qr = gfx_qrcode_create(s_ui.gfx);
    if (s_ui.setup_qr) {
        /* Dark modules on white: some scanners reject the widget's default inverted code */
        gfx_qrcode_set_size(s_ui.setup_qr, SETUP_QR_SIZE);
        gfx_qrcode_set_ecc(s_ui.setup_qr, GFX_QRCODE_ECC_LOW);
        gfx_qrcode_set_color(s_ui.setup_qr, (gfx_color_t){ .full = 0x0000 });
        gfx_qrcode_set_bg_color(s_ui.setup_qr, (gfx_color_t){ .full = 0xFFFF });
        gfx_qrcode_set_data(s_ui.setup_qr, "WIFI:T:nopass;S:VoCat;;");
        gfx_obj_align(s_ui.setup_qr, GFX_ALIGN_CENTER, 0, -34);
        gfx_obj_set_visible(s_ui.setup_qr, false);
    }
    s_ui.setup_hint = make_label(&lv_font_montserrat_14, COLOR_DIM, 260, 20, GFX_ALIGN_CENTER, 0, 54, GFX_LABEL_LONG_CLIP);
    set_label_text(s_ui.setup_hint, "Scan, or join Wi-Fi:");
    s_ui.setup_ssid = make_label(&lv_font_montserrat_20, COLOR_TEXT, 240, 28, GFX_ALIGN_CENTER, 0, 78, GFX_LABEL_LONG_CLIP);
    s_ui.setup_address = make_label(&lv_font_montserrat_20, COLOR_ACCENT, 240, 28, GFX_ALIGN_CENTER, 0, 108, GFX_LABEL_LONG_CLIP);

#if BOARD_ENABLE_QUICK_KEYS
    /* Created last so the keys draw on top of the backdrop */
    esp_err_t keys_err = app_quick_keys_create(s_ui.gfx);
    if (keys_err != ESP_OK) {
        ESP_LOGW(TAG, "Quick keys unavailable: %s", esp_err_to_name(keys_err));
    }
    app_usb_hid_add_command_cb(usb_command);
#endif

    /* Start from the saved settings */
    app_settings_get_name(s_ui.shown_name, sizeof(s_ui.shown_name));
    set_label_text(s_ui.name_label, s_ui.shown_name);
    s_ui.shown_settings = app_settings_version();
    app_display_set_backlight(app_settings_get_brightness());

    gfx_timer_handle_t timer = gfx_timer_create(s_ui.gfx, ui_tick, UI_TICK_MS, NULL);

    gfx_emote_unlock(s_ui.gfx);

    ESP_RETURN_ON_FALSE(timer, ESP_FAIL, TAG, "Failed to create UI timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL),
                        TAG, "Failed to register IP event handler");
    ESP_RETURN_ON_FALSE(xTaskCreate(data_task, "dashboard_data", 8192, NULL, 3, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create data task");

    esp_err_t battery_err = app_battery_init();
    if (battery_err != ESP_OK && battery_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Battery level unavailable: %s", esp_err_to_name(battery_err));
    }

    s_volume_queue = xQueueCreate(8, sizeof(int8_t));
    ESP_RETURN_ON_FALSE(s_volume_queue, ESP_ERR_NO_MEM, TAG, "Failed to create volume queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(volume_task, "dashboard_volume", 6144, NULL, 3, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create volume task");

    ESP_LOGI(TAG, "Dashboard initialized");
    return ESP_OK;
}

void app_dashboard_notice(const char *text, int seconds)
{
    if (!s_shared.lock) {
        return;
    }
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    strlcpy(s_shared.notice, text ? text : "", sizeof(s_shared.notice));
    s_shared.notice_until_us = text && seconds > 0 ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    s_shared.notice_dirty = true;
    xSemaphoreGive(s_shared.lock);
}

void app_dashboard_configure(const app_dashboard_config_t *config)
{
    if (config) {
        s_cfg = *config;
    }
}

void app_dashboard_set_wifi_setup(const char *ssid, const char *address)
{
    if (!s_shared.lock) {
        return;
    }
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    strlcpy(s_shared.setup_ssid, ssid ? ssid : "", sizeof(s_shared.setup_ssid));
    strlcpy(s_shared.setup_address, address ? address : "", sizeof(s_shared.setup_address));
    s_shared.setup_dirty = true;
    xSemaphoreGive(s_shared.lock);
}

void app_dashboard_on_state(app_device_system_state_t state)
{
    if (!s_shared.lock) {
        return;
    }
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    switch (state) {
        case APP_DEVICE_SYSTEM_STATE_LISTENING:
            copy_str(s_shared.status_text, sizeof(s_shared.status_text), "Listening...");
            s_shared.user_text[0] = '\0';
            s_shared.assistant_text[0] = '\0';
            s_shared.requested_page = PAGE_CONVERSATION;
            s_shared.return_at_us = 0;
            s_shared.text_dirty = true;
            break;
        case APP_DEVICE_SYSTEM_STATE_SLEEP:
            s_shared.return_at_us = esp_timer_get_time() + SLEEP_RETURN_US;
            break;
        case APP_DEVICE_SYSTEM_STATE_ACTIVE:
        default:
            s_shared.return_at_us = 0;
            break;
    }
    xSemaphoreGive(s_shared.lock);
}

void app_dashboard_on_text(app_device_text_type_t type, const char *text)
{
    if (!s_shared.lock || !text) {
        return;
    }
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    switch (type) {
        case APP_DEVICE_TEXT_TYPE_USER:
            snprintf(s_shared.user_text, sizeof(s_shared.user_text), "\"%s\"", text);
            s_shared.assistant_text[0] = '\0';
            copy_str(s_shared.status_text, sizeof(s_shared.status_text), "Thinking...");
            s_shared.requested_page = PAGE_CONVERSATION;
            break;
        case APP_DEVICE_TEXT_TYPE_ASSISTANT:
            copy_str(s_shared.assistant_text, sizeof(s_shared.assistant_text), text);
            copy_str(s_shared.status_text, sizeof(s_shared.status_text), DEVICE_NAME);
            s_shared.requested_page = PAGE_CONVERSATION;
            break;
        case APP_DEVICE_TEXT_TYPE_SYSTEM:
        default:
            copy_str(s_shared.status_text, sizeof(s_shared.status_text), text);
            break;
    }
    s_shared.return_at_us = 0;
    s_shared.text_dirty = true;
    xSemaphoreGive(s_shared.lock);
}

/* A tap on either side of the volume row (timers page) steps the playback volume. */
static bool volume_tap(uint16_t x, uint16_t y)
{
    if (s_ui.page != PAGE_TIMERS || !s_cfg.set_volume || y < VOLUME_ROW_Y_MIN || y > VOLUME_ROW_Y_MAX) {
        return false;
    }
    int delta;
    if (x < VOLUME_TAP_LEFT_X) {
        delta = -VOLUME_STEP;
    } else if (x > VOLUME_TAP_RIGHT_X) {
        delta = VOLUME_STEP;
    } else {
        return false;
    }

    int8_t step = (int8_t)delta;
    if (s_volume_queue) {
        xQueueSend(s_volume_queue, &step, 0);
    }
    return true;
}

bool app_dashboard_on_touch(bool pressed, uint16_t x, uint16_t y)
{
    bool on_keys = s_ui.visible && key_page(s_ui.page) >= 0;
    bool on_neurons = s_ui.visible && s_ui.page == PAGE_NEURONS;
    if (pressed) {
        s_ui.touch_x0 = x;
        s_ui.touch_y0 = y;
        s_ui.touch_down = true;
#if BOARD_ENABLE_QUICK_KEYS
        if (on_keys) {
            app_quick_keys_on_press(x, y);
        }
#endif
        return on_keys || on_neurons;   /* keeps the hold timers (factory reset, forget Wi-Fi) off */
    }
    if (!s_ui.touch_down) {
        return false;
    }
    s_ui.touch_down = false;
    if (!s_ui.visible) {
        return false;
    }

    int dx = (int)x - (int)s_ui.touch_x0;
    bool swiped = abs(dx) >= SWIPE_MIN_PX;
#if BOARD_ENABLE_QUICK_KEYS
    if (on_keys) {
        app_quick_keys_on_release(x, y, swiped);
    }
#endif
    if (!swiped) {
        if (on_neurons) {
            app_neurons_tap();
            return true;
        }
        return on_keys || volume_tap(s_ui.touch_x0, s_ui.touch_y0);
    }
    int page = next_page(s_ui.page, dx < 0 ? 1 : -1);
    if (s_shared.lock && xSemaphoreTake(s_shared.lock, portMAX_DELAY) == pdTRUE) {
        s_shared.requested_page = page;
        s_shared.return_at_us = 0;
        xSemaphoreGive(s_shared.lock);
    }
    return true;
}

void app_dashboard_add_timer(const char *label, int seconds)
{
    if (!s_shared.lock || seconds <= 0) {
        return;
    }
    xSemaphoreTake(s_shared.lock, portMAX_DELAY);
    int slot = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_shared.timers[i].end_us) {
            slot = i;
            break;
        }
        if (s_shared.timers[i].end_us < s_shared.timers[slot].end_us) {
            slot = i;   /* all busy: replace the one ending soonest */
        }
    }
    copy_str(s_shared.timers[slot].label, sizeof(s_shared.timers[slot].label), label ? label : "Timer");
    s_shared.timers[slot].end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    xSemaphoreGive(s_shared.lock);
}

#endif /* BOARD_ENABLE_DASHBOARD */
