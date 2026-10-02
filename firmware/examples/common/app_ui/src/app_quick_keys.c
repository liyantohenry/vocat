/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Quick keys: dashboard pages that turn the round screen into a key deck for the computer the board is
 * plugged into (sent as a USB keyboard by app_usb_hid.c).
 *
 * Each page is a click wheel: a ring split into four zones (top, right, bottom, left) around a glowing
 * centre button. Media keys are HID consumer usages, the same on every OS; holding a volume zone repeats
 * it. Desktop keys are key chords, one per host OS. The first time the Desktop page opens it asks which
 * computer it is plugged into; holding its centre button asks again. While the computer isn't connected
 * the icons dim and the centre says why.
 *
 * Drawing: the ring, the four pressed-zone overlays and the centre button are images generated once into
 * PSRAM. The ring and overlays are opaque (the page background is black), each overlay a copy of its part
 * of the ring with that zone tinted, so a press redraws only the overlay's rectangle. Draw order: ring,
 * overlays, centre button, labels.
 *
 * Threading: the widgets are created under the gfx lock and afterwards touched only in the gfx task
 * (dashboard ui_tick, touch callback and the repeat timer). Which computer the keys are for lives in
 * app_settings, which writes it to flash from its own task (the gfx task's stack is in PSRAM, so it
 * must not write flash itself).
 */

#include "app_quick_keys.h"

#if BOARD_ENABLE_QUICK_KEYS

#include <math.h>
#include <string.h>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "gfx.h"
#include "tusb.h"           /* HID_KEY_* keyboard usages */
#include "app_settings.h"
#include "app_usb_hid.h"

static const char *TAG = "app_quick_keys";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_48);

enum {
    SLOT_TOP = 0,
    SLOT_RIGHT,
    SLOT_BOTTOM,
    SLOT_LEFT,
    SLOT_CENTRE,
    SLOT_COUNT,
};
#define ZONE_COUNT          4       /* the ring zones are slots 0..3 */

/* Geometry, px from the screen centre (180, 180) */
#define SCREEN_C            180
#define RING_OUT            172     /* ring outer radius */
#define RING_IN             68      /* ring inner radius */
#define GAP_HALF            1.5f    /* half width of the diagonal gaps between zones */
#define ICON_R              120     /* icon distance from the centre */
#define BUTTON_R            54      /* centre button radius */
#define GLOW_R              64      /* the button's glow fades out here */
#define BUTTON_HIT_R        61      /* touches closer than this press the button, farther ones a zone */
#define ZONE_NEAR           47      /* overlay rectangles: from here out to RING_OUT along the zone's axis */
#define ZONE_HALF           122     /* ... and this far either side of it */
#define ZONE_LABEL_W        110     /* width of a zone's icon label */
#define CENTRE_LABEL_W      96

#define CHOOSE_HOLD_US      (1000 * 1000)   /* hold the Desktop centre button this long to pick the OS again */
#define REPEAT_DELAY_MS     400
#define REPEAT_PERIOD_MS    120

/* Colours */
#define RING_TOP            0x2B3744    /* ring surface, lit from above */
#define RING_BOTTOM         0x161D25
#define RING_RIM            0x40505F
#define ACCENT              0x5EC8F2
#define ZONE_TINT           0.35f       /* how much accent a pressed zone takes */
#define BUTTON_TOP          0x8BDDF8
#define BUTTON_BOTTOM       0x36ACDE
#define BUTTON_DOWN_TOP     0x5CC5EE
#define BUTTON_DOWN_BOTTOM  0x2790BE
#define BUTTON_OFF          0x1A232B
#define ICON                0xE8EEF3
#define ICON_DOWN           0xFFFFFF
#define ICON_OFF            0x3C4651
#define CAPTION             0xA9B5C2
#define CAPTION_DOWN        0xD8F3FF
#define CAPTION_OFF         0x333C45
#define ON_BUTTON           0x03222F
#define BUTTON_OFF_TEXT     0x56616D

/* Unit vectors of the zone axes (screen y points down) */
static const int8_t s_axis[ZONE_COUNT][2] = {
    [SLOT_TOP] = { 0, -1 },
    [SLOT_RIGHT] = { 1, 0 },
    [SLOT_BOTTOM] = { 0, 1 },
    [SLOT_LEFT] = { -1, 0 },
};

/* ------------------------------------------------------------------------------------------------
 * Key definitions
 * ---------------------------------------------------------------------------------------------- */

/* Which computer the chords are for, kept in app_settings so the chooser on the screen and the settings
 * page change the same setting. Linux means GNOME defaults.
 */
#define HOST_WINDOWS    APP_SETTINGS_OS_WINDOWS
#define HOST_MACOS      APP_SETTINGS_OS_MACOS
#define HOST_LINUX      APP_SETTINGS_OS_LINUX
#define HOST_COUNT      APP_SETTINGS_OS_COUNT
#define HOST_UNSET      APP_SETTINGS_OS_UNSET
typedef app_settings_os_t host_os_t;

/* Where each OS appears when the Desktop page asks for it (-1 = zone not used) */
static const int8_t s_choice_host[SLOT_COUNT] = {
    [SLOT_TOP] = -1,
    [SLOT_RIGHT] = HOST_MACOS,
    [SLOT_BOTTOM] = HOST_LINUX,
    [SLOT_LEFT] = HOST_WINDOWS,
    [SLOT_CENTRE] = -1,
};

typedef enum {
    ACT_CONSUMER,
    ACT_CHORD,
} action_type_t;

typedef struct {
    uint8_t mods;
    uint8_t key;    /* HID keyboard usage, 0 = modifiers only */
} chord_t;

typedef struct {
    const char *name;               /* for the log */
    const char *icon;               /* LV_SYMBOL_* glyph */
    const char *caption;            /* word under the icon, or NULL for an icon-only key */
    action_type_t type;
    bool repeat;                    /* repeats while held */
    uint16_t usage;                 /* ACT_CONSUMER: consumer page usage */
    chord_t chord[HOST_COUNT];      /* ACT_CHORD: per OS */
} key_def_t;

typedef struct {
    bool per_os;                    /* chords depend on the host OS */
    key_def_t keys[SLOT_COUNT];
} page_def_t;

#define CTRL    APP_USB_MOD_CTRL
#define SHIFT   APP_USB_MOD_SHIFT
#define GUI     APP_USB_MOD_GUI
#define K(mods_, key_)  { (mods_), (key_) }

static const page_def_t s_pages[APP_QUICK_KEYS_PAGE_COUNT] = {
    {   /* Media */
        .keys = {
            [SLOT_CENTRE] = { "Play/Pause", LV_SYMBOL_PLAY LV_SYMBOL_PAUSE, NULL, ACT_CONSUMER, false, 0x0CD },
            [SLOT_TOP] = { "Volume up", LV_SYMBOL_VOLUME_MAX, NULL, ACT_CONSUMER, true, 0x0E9 },
            [SLOT_BOTTOM] = { "Volume down", LV_SYMBOL_VOLUME_MID, NULL, ACT_CONSUMER, true, 0x0EA },
            [SLOT_LEFT] = { "Previous", LV_SYMBOL_PREV, NULL, ACT_CONSUMER, false, 0x0B6 },
            [SLOT_RIGHT] = { "Next", LV_SYMBOL_NEXT, NULL, ACT_CONSUMER, false, 0x0B5 },
        },
    },
    {   /* Desktop: Windows, macOS, Linux (GNOME) */
        .per_os = true,
        .keys = {
            [SLOT_CENTRE] = { "Overview", LV_SYMBOL_LIST, NULL, ACT_CHORD, false, 0,
                              { K(GUI, HID_KEY_TAB), K(CTRL, HID_KEY_ARROW_UP), K(GUI, 0) } },
            [SLOT_TOP] = { "Screenshot", LV_SYMBOL_IMAGE, "Capture", ACT_CHORD, false, 0,
                           { K(GUI | SHIFT, HID_KEY_S), K(GUI | SHIFT, HID_KEY_4), K(0, HID_KEY_PRINT_SCREEN) } },
            [SLOT_BOTTOM] = { "Show desktop", LV_SYMBOL_HOME, "Desktop", ACT_CHORD, false, 0,
                              { K(GUI, HID_KEY_D), K(0, HID_KEY_F11), K(GUI, HID_KEY_D) } },
            [SLOT_LEFT] = { "Previous desktop", LV_SYMBOL_LEFT, "Prev desk", ACT_CHORD, false, 0,
                            { K(GUI | CTRL, HID_KEY_ARROW_LEFT), K(CTRL, HID_KEY_ARROW_LEFT), K(GUI, HID_KEY_PAGE_UP) } },
            [SLOT_RIGHT] = { "Next desktop", LV_SYMBOL_RIGHT, "Next desk", ACT_CHORD, false, 0,
                             { K(GUI | CTRL, HID_KEY_ARROW_RIGHT), K(CTRL, HID_KEY_ARROW_RIGHT), K(GUI, HID_KEY_PAGE_DOWN) } },
        },
    },
};

/* ------------------------------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------------------------- */

typedef enum {
    BUTTON_NORMAL,
    BUTTON_PRESSED,
    BUTTON_DIMMED,
    BUTTON_LOOKS,
} button_look_t;

static struct {
    gfx_image_dsc_t ring;
    gfx_image_dsc_t zone[ZONE_COUNT];           /* ring rectangle with that zone tinted */
    int16_t zone_x[ZONE_COUNT], zone_y[ZONE_COUNT];   /* overlay top-left, screen px */
    gfx_image_dsc_t button[BUTTON_LOOKS];
} s_img;

static struct {
    gfx_handle_t gfx;
    gfx_obj_t *ring;
    gfx_obj_t *zone[ZONE_COUNT];
    gfx_obj_t *button;
    gfx_obj_t *icon[SLOT_COUNT];
    gfx_obj_t *caption[SLOT_COUNT];
    gfx_obj_t *prompt;              /* "Which computer?" while the Desktop page asks for the OS */
    gfx_timer_handle_t repeat_timer;
    int page;                       /* -1 = hidden */
    bool choosing;                  /* the Desktop page is asking for the OS */
    int usb_shown;                  /* USB status the page was drawn for, -1 = not drawn */
    int pressed_slot;               /* -1 = none */
    int64_t pressed_at_us;
    int repeats;                    /* times the held key has repeated */
} s_ui = { .page = -1, .usb_shown = -1, .pressed_slot = -1 };


/* ------------------------------------------------------------------------------------------------
 * Image generation
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
    float r, g, b;
} rgbf_t;

static rgbf_t rgb(uint32_t hex)
{
    return (rgbf_t) { (float)((hex >> 16) & 0xFF), (float)((hex >> 8) & 0xFF), (float)(hex & 0xFF) };
}

static rgbf_t mix(rgbf_t a, rgbf_t b, float t)    /* t = 0 gives a, 1 gives b */
{
    return (rgbf_t) { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static rgbf_t scale(rgbf_t c, float k)
{
    return (rgbf_t) { c.r * k, c.g * k, c.b * k };
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* RGB565, byte-swapped: the display runs with swap on, and images are copied into the buffer as-is */
static uint16_t to565(rgbf_t c)
{
    uint8_t r = (uint8_t)(clamp01(c.r / 255.0f) * 255.0f + 0.5f);
    uint8_t g = (uint8_t)(clamp01(c.g / 255.0f) * 255.0f + 0.5f);
    uint8_t b = (uint8_t)(clamp01(c.b / 255.0f) * 255.0f + 0.5f);
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v << 8) | (v >> 8));
}

/* The zone a point belongs to by angle (x, y relative to the centre, y down) */
static int zone_of(float x, float y)
{
    if (fabsf(y) >= fabsf(x)) {
        return y < 0 ? SLOT_TOP : SLOT_BOTTOM;
    }
    return x > 0 ? SLOT_RIGHT : SLOT_LEFT;
}

/* Ring colour over the black background at a point, with zone `tinted` (or -1) in the pressed colour */
static rgbf_t ring_pixel(float x, float y, int tinted)
{
    float r = sqrtf(x * x + y * y);
    float cover = clamp01(RING_OUT - r + 0.5f) * clamp01(r - RING_IN + 0.5f);
    /* Diagonal gaps: distance to the lines y = x and y = -x */
    float gap = fminf(fabsf(x - y), fabsf(x + y)) * 0.70710678f;
    cover *= clamp01(gap - GAP_HALF + 0.5f);
    if (cover <= 0.0f) {
        return (rgbf_t) { 0, 0, 0 };
    }

    rgbf_t c = mix(rgb(RING_TOP), rgb(RING_BOTTOM), clamp01((y + RING_OUT) / (2.0f * RING_OUT)));
    if (tinted >= 0 && zone_of(x, y) == tinted) {
        c = mix(c, rgb(ACCENT), ZONE_TINT);
    }
    /* A thin lighter rim along both edges gives the ring its outline */
    float rim = fmaxf(clamp01(1.6f - (RING_OUT - r)), clamp01(1.6f - (r - RING_IN)));
    c = mix(c, rgb(RING_RIM), rim * 0.8f);
    return scale(c, cover);
}

static esp_err_t alloc_image(gfx_image_dsc_t *dsc, uint16_t w, uint16_t h, bool alpha)
{
    size_t size = (size_t)w * h * (alpha ? 3 : 2);
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(data, ESP_ERR_NO_MEM, TAG, "No memory for a %ux%u image", w, h);
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = C_ARRAY_HEADER_MAGIC;
    dsc->header.cf = alpha ? GFX_COLOR_FORMAT_RGB565A8 : GFX_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = size;
    dsc->data = data;
    return ESP_OK;
}

/* Opaque ring rectangle with top-left at screen (x0, y0) */
static esp_err_t make_ring_rect(gfx_image_dsc_t *dsc, int x0, int y0, uint16_t w, uint16_t h, int tinted)
{
    ESP_RETURN_ON_ERROR(alloc_image(dsc, w, h, false), TAG, "ring");
    uint16_t *px = (uint16_t *)dsc->data;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            px[j * w + i] = to565(ring_pixel(x0 + i + 0.5f - SCREEN_C, y0 + j + 0.5f - SCREEN_C, tinted));
        }
    }
    return ESP_OK;
}

/* Centre button with its glow, RGB565 + alpha so the ring shows around it */
static esp_err_t make_button(gfx_image_dsc_t *dsc, button_look_t look)
{
    const uint16_t d = 2 * GLOW_R;
    ESP_RETURN_ON_ERROR(alloc_image(dsc, d, d, true), TAG, "button");
    uint16_t *px = (uint16_t *)dsc->data;
    uint8_t *alpha = (uint8_t *)dsc->data + (size_t)d * d * 2;
    float glow_peak = look == BUTTON_PRESSED ? 0.6f : (look == BUTTON_NORMAL ? 0.4f : 0.0f);

    for (int j = 0; j < d; j++) {
        for (int i = 0; i < d; i++) {
            float x = i + 0.5f - GLOW_R;
            float y = j + 0.5f - GLOW_R;
            float r = sqrtf(x * x + y * y);
            float t = clamp01((y + BUTTON_R) / (2.0f * BUTTON_R));

            rgbf_t face;
            if (look == BUTTON_DIMMED) {
                face = mix(rgb(BUTTON_OFF), rgb(RING_RIM), clamp01(1.6f - (BUTTON_R - r)) * 0.8f);
            } else if (look == BUTTON_PRESSED) {
                face = mix(rgb(BUTTON_DOWN_TOP), rgb(BUTTON_DOWN_BOTTOM), t);
            } else {
                face = mix(rgb(BUTTON_TOP), rgb(BUTTON_BOTTOM), t);
            }
            float face_a = clamp01(BUTTON_R - r + 0.5f);
            float fade = clamp01((GLOW_R - r) / (GLOW_R - BUTTON_R));
            float glow_a = glow_peak * fade * fade;

            /* Face over glow */
            float a = face_a + glow_a * (1.0f - face_a);
            rgbf_t c = a > 0.0f ? mix(rgb(ACCENT), face, face_a / a) : rgb(ACCENT);
            size_t k = (size_t)j * d + i;
            px[k] = to565(c);
            alpha[k] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
    return ESP_OK;
}

static esp_err_t make_images(void)
{
    ESP_RETURN_ON_ERROR(make_ring_rect(&s_img.ring, SCREEN_C - RING_OUT, SCREEN_C - RING_OUT,
                                       2 * RING_OUT, 2 * RING_OUT, -1), TAG, "ring");
    for (int z = 0; z < ZONE_COUNT; z++) {
        /* The rectangle from ZONE_NEAR to RING_OUT along the zone's axis, ZONE_HALF either side */
        int ax = s_axis[z][0], ay = s_axis[z][1];
        int x0, y0, w, h;
        if (ax == 0) {
            w = 2 * ZONE_HALF;
            h = RING_OUT - ZONE_NEAR;
            x0 = SCREEN_C - ZONE_HALF;
            y0 = ay < 0 ? SCREEN_C - RING_OUT : SCREEN_C + ZONE_NEAR;
        } else {
            w = RING_OUT - ZONE_NEAR;
            h = 2 * ZONE_HALF;
            x0 = ax < 0 ? SCREEN_C - RING_OUT : SCREEN_C + ZONE_NEAR;
            y0 = SCREEN_C - ZONE_HALF;
        }
        s_img.zone_x[z] = x0;
        s_img.zone_y[z] = y0;
        ESP_RETURN_ON_ERROR(make_ring_rect(&s_img.zone[z], x0, y0, w, h, z), TAG, "zone");
    }
    for (int look = 0; look < BUTTON_LOOKS; look++) {
        ESP_RETURN_ON_ERROR(make_button(&s_img.button[look], look), TAG, "button");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------------------
 * Drawing (gfx task)
 * ---------------------------------------------------------------------------------------------- */

static gfx_obj_t *make_label(const lv_font_t *font, uint16_t w, uint16_t h)
{
    gfx_obj_t *label = gfx_label_create(s_ui.gfx);
    if (!label) {
        return NULL;
    }
    gfx_label_set_font(label, (gfx_font_t)font);
    gfx_label_set_text_align(label, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(label, GFX_LABEL_LONG_CLIP);
    gfx_obj_set_size(label, w, h);
    gfx_label_set_text(label, "");
    gfx_obj_set_visible(label, false);
    return label;
}

/* Labels draw their text from the top of the box, so a label is only centred on its position when the box
 * is as tall as the font's line: set the height whenever the font changes.
 */
static void set_font(gfx_obj_t *label, const lv_font_t *font, uint16_t width)
{
    gfx_label_set_font(label, (gfx_font_t)font);
    gfx_obj_set_size(label, width, font->line_height);
}

static void show_text(gfx_obj_t *label, const char *text, uint32_t color, int dx, int dy)
{
    gfx_label_set_text(label, text);
    gfx_label_set_color(label, GFX_COLOR_HEX(color));
    gfx_obj_align(label, GFX_ALIGN_CENTER, dx, dy);
    gfx_obj_set_visible(label, true);
}

static void hide_all(void)
{
    gfx_obj_set_visible(s_ui.ring, false);
    gfx_obj_set_visible(s_ui.button, false);
    gfx_obj_set_visible(s_ui.prompt, false);
    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        if (slot < ZONE_COUNT) {
            gfx_obj_set_visible(s_ui.zone[slot], false);
        }
        gfx_obj_set_visible(s_ui.icon[slot], false);
        gfx_obj_set_visible(s_ui.caption[slot], false);
    }
}

/* USB status as a small number so the tick can tell when it changes: 0 = ready */
static int usb_status(const char **why)
{
    switch (app_usb_hid_get_state()) {
        case APP_USB_HID_STATE_WAITING:
            *why = "Starting";
            return 1;
        case APP_USB_HID_STATE_JTAG:
            *why = "Flash mode";
            return 2;
        case APP_USB_HID_STATE_ACTIVE:
        default:
            if (!app_usb_hid_host_ready()) {
                *why = "No computer";
                return 3;
            }
            *why = NULL;
            return 0;
    }
}

static bool keys_ready(void)
{
    const char *why;
    return usb_status(&why) == 0;
}

/* Icon and caption of a ring zone */
static void draw_zone_text(int slot, bool pressed)
{
    int x = s_axis[slot][0] * ICON_R;
    int y = s_axis[slot][1] * ICON_R;

    if (s_ui.choosing) {
        int host = s_choice_host[slot];
        gfx_obj_set_visible(s_ui.caption[slot], false);
        if (host < 0) {
            gfx_obj_set_visible(s_ui.icon[slot], false);
            return;
        }
        set_font(s_ui.icon[slot], &lv_font_montserrat_20, ZONE_LABEL_W);
        show_text(s_ui.icon[slot], app_settings_os_name(host), pressed ? ICON_DOWN : ICON, x, y);
        return;
    }

    const key_def_t *def = &s_pages[s_ui.page].keys[slot];
    bool ready = keys_ready();
    uint32_t icon = !ready ? ICON_OFF : (pressed ? ICON_DOWN : ICON);
    uint32_t caption = !ready ? CAPTION_OFF : (pressed ? CAPTION_DOWN : CAPTION);
    set_font(s_ui.icon[slot], &lv_font_montserrat_48, ZONE_LABEL_W);
    show_text(s_ui.icon[slot], def->icon, icon, x, def->caption ? y - 12 : y);
    if (def->caption) {
        show_text(s_ui.caption[slot], def->caption, caption, x, y + 26);
    } else {
        gfx_obj_set_visible(s_ui.caption[slot], false);
    }
}

static void draw_button(bool pressed)
{
    if (s_ui.choosing) {
        /* A dimmed button with a keyboard: part of the wheel, but not something to press */
        gfx_img_set_src(s_ui.button, &s_img.button[BUTTON_DIMMED]);
        gfx_obj_set_pos(s_ui.button, SCREEN_C - GLOW_R, SCREEN_C - GLOW_R);
        gfx_obj_set_visible(s_ui.button, true);
        set_font(s_ui.icon[SLOT_CENTRE], &lv_font_montserrat_48, CENTRE_LABEL_W);
        show_text(s_ui.icon[SLOT_CENTRE], LV_SYMBOL_KEYBOARD, BUTTON_OFF_TEXT, 0, 0);
        return;
    }
    const char *why = NULL;
    bool ready = usb_status(&why) == 0;
    gfx_img_set_src(s_ui.button, &s_img.button[!ready ? BUTTON_DIMMED : (pressed ? BUTTON_PRESSED : BUTTON_NORMAL)]);
    gfx_obj_set_pos(s_ui.button, SCREEN_C - GLOW_R, SCREEN_C - GLOW_R);
    gfx_obj_set_visible(s_ui.button, true);
    if (ready) {
        /* A single glyph (3 UTF-8 bytes) fills the button at 48 px; a pair like play/pause needs 28 px */
        const char *icon = s_pages[s_ui.page].keys[SLOT_CENTRE].icon;
        const lv_font_t *font = strlen(icon) > 3 ? &lv_font_montserrat_28 : &lv_font_montserrat_48;
        set_font(s_ui.icon[SLOT_CENTRE], font, CENTRE_LABEL_W);
        show_text(s_ui.icon[SLOT_CENTRE], icon, ON_BUTTON, 0, 0);
    } else {
        /* The dimmed button says why the keys are off */
        set_font(s_ui.icon[SLOT_CENTRE], &lv_font_montserrat_14, CENTRE_LABEL_W);
        show_text(s_ui.icon[SLOT_CENTRE], why, BUTTON_OFF_TEXT, 0, 0);
    }
}

static void draw_pressed(int slot, bool pressed)
{
    if (slot == SLOT_CENTRE) {
        draw_button(pressed);
        return;
    }
    gfx_obj_set_visible(s_ui.zone[slot], pressed);
    draw_zone_text(slot, pressed);
}

static void draw_page(void)
{
    const char *why;
    s_ui.usb_shown = usb_status(&why);
    s_ui.choosing = s_pages[s_ui.page].per_os && app_settings_get_os() == HOST_UNSET;

    gfx_obj_set_visible(s_ui.ring, true);
    for (int z = 0; z < ZONE_COUNT; z++) {
        gfx_obj_set_visible(s_ui.zone[z], false);
        draw_zone_text(z, false);
    }
    draw_button(false);
    gfx_obj_set_visible(s_ui.prompt, s_ui.choosing);
}

/* The zone or button under a touch, or -1 */
static int slot_at(uint16_t x, uint16_t y)
{
    float dx = x + 0.5f - SCREEN_C;
    float dy = y + 0.5f - SCREEN_C;
    float r = sqrtf(dx * dx + dy * dy);
    int slot;
    if (r < BUTTON_HIT_R) {
        slot = SLOT_CENTRE;
    } else if (r <= SCREEN_C) {
        slot = zone_of(dx, dy);
    } else {
        return -1;
    }

    if (s_ui.choosing) {
        return s_choice_host[slot] >= 0 ? slot : -1;
    }
    return keys_ready() ? slot : -1;
}

/* ------------------------------------------------------------------------------------------------
 * Actions
 * ---------------------------------------------------------------------------------------------- */

static void fire(const key_def_t *def)
{
    esp_err_t err;
    if (def->type == ACT_CONSUMER) {
        err = app_usb_hid_send_consumer(def->usage);
    } else {
        host_os_t os = app_settings_get_os();
        const chord_t *c = &def->chord[os < HOST_COUNT ? os : HOST_WINDOWS];
        err = app_usb_hid_send_chord(c->mods, c->key);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s not sent: %s", def->name, esp_err_to_name(err));
    }
}

static void repeat_cb(void *arg)
{
    (void)arg;
    if (s_ui.page < 0 || s_ui.pressed_slot < 0 || s_ui.choosing) {
        gfx_timer_pause(s_ui.repeat_timer);
        return;
    }
    fire(&s_pages[s_ui.page].keys[s_ui.pressed_slot]);
    s_ui.repeats++;
    gfx_timer_set_period(s_ui.repeat_timer, REPEAT_PERIOD_MS);
}

/* ------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------- */

esp_err_t app_quick_keys_create(void *gfx_handle)
{
    ESP_RETURN_ON_FALSE(gfx_handle, ESP_ERR_INVALID_ARG, TAG, "No gfx handle");
    ESP_RETURN_ON_FALSE(s_ui.gfx == NULL, ESP_ERR_INVALID_STATE, TAG, "Already created");
    s_ui.gfx = (gfx_handle_t)gfx_handle;

    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(make_images(), TAG, "Failed to draw the key images");
    ESP_LOGI(TAG, "Key images drawn in %d ms, %u KB PSRAM left", (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* Created in drawing order: ring, zone overlays, button, then the labels on top */
    s_ui.ring = gfx_img_create(s_ui.gfx);
    ESP_RETURN_ON_FALSE(s_ui.ring, ESP_ERR_NO_MEM, TAG, "Failed to create the ring");
    gfx_img_set_src(s_ui.ring, &s_img.ring);
    gfx_obj_set_pos(s_ui.ring, SCREEN_C - RING_OUT, SCREEN_C - RING_OUT);
    gfx_obj_set_visible(s_ui.ring, false);
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_ui.zone[z] = gfx_img_create(s_ui.gfx);
        ESP_RETURN_ON_FALSE(s_ui.zone[z], ESP_ERR_NO_MEM, TAG, "Failed to create a zone");
        gfx_img_set_src(s_ui.zone[z], &s_img.zone[z]);
        gfx_obj_set_pos(s_ui.zone[z], s_img.zone_x[z], s_img.zone_y[z]);
        gfx_obj_set_visible(s_ui.zone[z], false);
    }
    s_ui.button = gfx_img_create(s_ui.gfx);
    ESP_RETURN_ON_FALSE(s_ui.button, ESP_ERR_NO_MEM, TAG, "Failed to create the button");
    gfx_img_set_src(s_ui.button, &s_img.button[BUTTON_NORMAL]);
    gfx_obj_set_pos(s_ui.button, SCREEN_C - GLOW_R, SCREEN_C - GLOW_R);
    gfx_obj_set_visible(s_ui.button, false);

    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        s_ui.icon[slot] = make_label(&lv_font_montserrat_28, slot == SLOT_CENTRE ? CENTRE_LABEL_W : ZONE_LABEL_W,
                                     lv_font_montserrat_28.line_height);
        s_ui.caption[slot] = make_label(&lv_font_montserrat_14, 96, lv_font_montserrat_14.line_height);
        ESP_RETURN_ON_FALSE(s_ui.icon[slot] && s_ui.caption[slot], ESP_ERR_NO_MEM, TAG, "Failed to create labels");
    }
    s_ui.prompt = make_label(&lv_font_montserrat_14, 140, lv_font_montserrat_14.line_height);
    ESP_RETURN_ON_FALSE(s_ui.prompt, ESP_ERR_NO_MEM, TAG, "Failed to create the prompt");
    gfx_label_set_text(s_ui.prompt, "Which computer?");
    gfx_label_set_color(s_ui.prompt, GFX_COLOR_HEX(CAPTION));
    gfx_obj_align(s_ui.prompt, GFX_ALIGN_CENTER, 0, -ICON_R);

    s_ui.repeat_timer = gfx_timer_create(s_ui.gfx, repeat_cb, REPEAT_DELAY_MS, NULL);
    ESP_RETURN_ON_FALSE(s_ui.repeat_timer, ESP_ERR_NO_MEM, TAG, "Failed to create repeat timer");
    gfx_timer_pause(s_ui.repeat_timer);

    ESP_LOGI(TAG, "Quick keys ready (computer: %s)", app_settings_os_name(app_settings_get_os()));
    return app_usb_hid_init();
}

void app_quick_keys_show(int page)
{
    if (!s_ui.gfx) {
        return;
    }
    gfx_timer_pause(s_ui.repeat_timer);
    s_ui.pressed_slot = -1;
    if (page < 0 || page >= APP_QUICK_KEYS_PAGE_COUNT) {
        s_ui.page = -1;
        hide_all();
        return;
    }
    s_ui.page = page;
    draw_page();
}

void app_quick_keys_on_press(uint16_t x, uint16_t y)
{
    if (s_ui.page < 0) {
        return;
    }
    int slot = slot_at(x, y);
    s_ui.pressed_slot = slot;
    s_ui.pressed_at_us = esp_timer_get_time();
    s_ui.repeats = 0;
    if (slot < 0) {
        return;
    }
    draw_pressed(slot, true);
    if (!s_ui.choosing && s_pages[s_ui.page].keys[slot].repeat) {
        gfx_timer_set_period(s_ui.repeat_timer, REPEAT_DELAY_MS);
        gfx_timer_reset(s_ui.repeat_timer);
        gfx_timer_resume(s_ui.repeat_timer);
    }
}

void app_quick_keys_on_release(uint16_t x, uint16_t y, bool swiped)
{
    gfx_timer_pause(s_ui.repeat_timer);
    int slot = s_ui.pressed_slot;
    s_ui.pressed_slot = -1;
    if (s_ui.page < 0 || slot < 0) {
        return;
    }
    draw_pressed(slot, false);
    if (swiped || slot_at(x, y) != slot) {
        return;     /* slid off the zone or swiped to another page */
    }

    if (s_ui.choosing) {
        app_settings_set_os((host_os_t)s_choice_host[slot]);
        draw_page();
        return;
    }
    int64_t held_us = esp_timer_get_time() - s_ui.pressed_at_us;
    if (slot == SLOT_CENTRE && s_pages[s_ui.page].per_os && held_us >= CHOOSE_HOLD_US) {
        app_settings_set_os(HOST_UNSET);    /* ask again */
        draw_page();
        return;
    }
    if (s_ui.repeats == 0) {
        fire(&s_pages[s_ui.page].keys[slot]);
    }
}

void app_quick_keys_tick(void)
{
    static uint32_t shown_settings;
    if (s_ui.page < 0 || s_ui.pressed_slot >= 0) {
        return;
    }
    const char *why;
    if (app_settings_version() != shown_settings) {
        shown_settings = app_settings_version();
        draw_page();    /* the settings page may have changed which computer the keys are for */
    } else if (!s_ui.choosing && usb_status(&why) != s_ui.usb_shown) {
        draw_page();    /* dim or light the keys as the computer comes and goes */
    }
}

#endif /* BOARD_ENABLE_QUICK_KEYS */
