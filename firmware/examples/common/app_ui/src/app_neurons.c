/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Neurons: a dashboard page with a random network of drifting nodes. Wires join whichever free nodes are
 * close, and signals fire along them. A tap plays the texts once: the name, then the Neurons texts from
 * app_settings. The nodes pull out of the network to write the first text in a 5 x 7 LED font, morph from
 * one text to the next, and return to drifting. Hold times vary by about 20% and the cascades start from
 * random nodes, so no two plays look the same.
 *
 * Drawing: every frame is drawn into an 8-bit intensity buffer (lines and dots add up), then turned into
 * one full-screen RGB565 image through a 256-entry colour table: low values are the page colour at that
 * brightness, and values past full brightness fade to white, which lights the firing nodes and signals.
 *
 * Threading: the simulation and drawing run in neurons_task on the other core from the gfx task, into
 * three screen images in turn: one on screen, one finished and waiting, one being drawn. The gfx task's
 * frame timer shows the newest finished image. Between the two tasks, only the fields marked shared
 * below are touched from both sides, and the image indices only under s_buf_lock. The texts and colour are read
 * from app_settings when they change, between plays.
 */

#include "app_neurons.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gfx.h"
#include "app_display.h"
#include "app_settings.h"

static const char *TAG = "app_neurons";

#define SIZE            360
#define CENTRE          180.0f
#define TAU             6.2831853f
#define FREE_R          165.0f      /* drifting nodes stay inside this radius */
#define HOME_R          150.0f      /* resting positions are spread inside this radius */
#define MOVE_S          1.4f        /* one node's move between two places */
#define SPAN_S          2.0f        /* one whole morph, with the nodes' staggered starts */
#define TAP_LEAD_S      0.15f       /* from the tap to the first nodes moving */
#define REFRACTORY_S    1.2f        /* a node fires at most this often */
#define SPARKS_PER_S    0.6f        /* random firings of the network */
#define PASS_NET        0.3f        /* chance a firing node sends a signal down each network wire */
#define PASS_LETTER     0.75f       /* ... and down each letter stroke */
#define FRAME_MS        10          /* how often the gfx task checks for a finished image */
#define MIN_FRAME_MS    30          /* the task draws at most this often */
#define N_IMAGES        3
#define TASK_CORE       1           /* the gfx task runs on core 0 */

#define MAX_TEXTS       (1 + APP_SETTINGS_TEXT_COUNT)
#define MAX_PHASES      (MAX_TEXTS + 2)     /* rest, the texts, back to rest */
#define MAX_PTS         720         /* dots in one text */
#define MAX_NODES       1000
#define MAX_EDGES       8192
#define MAX_PULSES      512
#define MAX_KICKS       (4 * MAX_TEXTS)
#define GRID_MAX        64

#define LEVEL           160.0f      /* intensity of full colour; above it the colour fades to white */

typedef struct {
    int16_t c, r;
    float x, y;
} pt_t;

typedef struct {
    int n_pts, n_links;
    float rad;                      /* dot radius at this text's dot pitch */
    pt_t *pts;
    int16_t (*links)[2];            /* neighbouring dots in a stroke */
    int16_t *order;                 /* dots from left to right */
} text_set_t;

typedef struct {
    float hx, hy;                   /* resting position */
    float ax, ay, wx, wy, phx, phy; /* drift: amplitude, speed and phase of the slow orbit */
    float curve;                    /* sideways bend of this node's moves */
    float x, y, rad, m, e;          /* now: position, radius, how far into a letter (0..1), move progress */
    float flash, last;              /* firing glow, and when it last fired */
    uint16_t adj_start, adj_count;
} node_t;

typedef struct {
    uint16_t a, b;
    float alpha;
    bool let;                       /* a letter stroke rather than a network wire */
} edge_t;

typedef struct {
    uint16_t from, to;
    bool let;
    float t0, dur;
} pulse_t;

typedef struct {
    float at;
    int16_t node;
} kick_t;

static struct {
    gfx_handle_t gfx;
    gfx_obj_t *img;
    gfx_timer_handle_t timer;
    gfx_image_dsc_t dsc[N_IMAGES];
    uint16_t *pixels[N_IMAGES];     /* the screen images, byte-swapped RGB565 */
    uint8_t *level;                 /* intensity buffer */
    uint16_t lut[256];
    TaskHandle_t task;
    bool level_internal;

    /* Shared between the gfx task and neurons_task */
    volatile bool shown;            /* set by the gfx task */
    volatile uint32_t show_count;   /* counts showings, so the task starts afresh each time */
    volatile bool pending_tap;      /* set by the gfx task, taken by the task */
    volatile bool active;           /* the texts are playing; set by the task */
    int ready;                      /* newest finished image, -1 = none (s_buf_lock) */
    int on_screen;                  /* image the gfx task shows (s_buf_lock) */

    /* The texts and the network, rebuilt when the settings change */
    uint32_t built_version;
    bool built;
    int n_texts;
    text_set_t sets[MAX_TEXTS];
    int total;
    node_t *nodes;
    float near, far;
    int16_t *cell_map;              /* layout scratch: dot index at (c, r) */

    /* The plan for one play, drawn at random on each tap */
    float T[MAX_PHASES + 1], cycle;
    int16_t *assign[MAX_PHASES];    /* per node: dot index in that phase's text, -1 = free */
    float *delay[MAX_PHASES];
    int16_t *node_of[MAX_PHASES];   /* per dot of that phase's text: its node */
    kick_t kicks[MAX_KICKS];
    int n_kicks;
    struct { float key; int16_t idx; } *sort_buf;
    float *pos_x, *pos_y;

    /* Per frame */
    edge_t *edges;
    int n_edges;
    uint16_t *adj;
    int16_t grid_head[GRID_MAX * GRID_MAX];
    int16_t *grid_next;
    pulse_t pulses[MAX_PULSES];
    int n_pulses;
    float shown_amount;             /* how much the texts are on screen (0..1): dims the free network */
    float wobble;

    /* Timing (neurons_task) */
    uint32_t seen_show_count;
    int64_t start_us, last_us;
    float clock, last_tc;
    int frames;
    int64_t time_place_us, time_draw_us, time_convert_us, first_frame_us;
    int16_t span_x0[SIZE], span_x1[SIZE];   /* per row, the pixels inside the round screen */
} s_n;

/* Disc coverage stamps, one per radius in quarter pixels, built when first needed */
#define STAMP_Q_MAX     40
#define STAMP_BYTES     (24 * 1024)
static struct {
    uint8_t data[STAMP_BYTES];
    uint16_t off[STAMP_Q_MAX + 1];
    uint8_t half[STAMP_Q_MAX + 1];  /* 0 = not built */
    size_t used;
} s_stamp;

static portMUX_TYPE s_buf_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------------- */

static float rnd(void)
{
    return (float)(esp_random() >> 8) * (1.0f / 16777216.0f);
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ease(float v)      /* cubic ease in and out */
{
    return v < 0.5f ? 4.0f * v * v * v : 1.0f - powf(-2.0f * v + 2.0f, 3.0f) / 2.0f;
}

static float vary(float base)   /* base give or take 20% */
{
    return base * (0.8f + rnd() * 0.4f);
}

static void *alloc_psram(size_t size)
{
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ------------------------------------------------------------------------------------------------
 * 5 x 7 LED font and text layout
 * ---------------------------------------------------------------------------------------------- */

static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.'!&?,@+/:";
static const uint8_t FONT_ROWS[][7] = {     /* one byte per row, bit 4 = leftmost column */
    {14, 17, 17, 17, 31, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {28, 18, 17, 17, 17, 18, 28},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 17, 10, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 31, 0, 0, 0}, {0, 0, 0, 0, 0, 12, 12}, {12, 4, 8, 0, 0, 0, 0},
    {4, 4, 4, 4, 4, 0, 4}, {12, 18, 20, 8, 21, 18, 13}, {14, 17, 1, 2, 4, 0, 4}, {0, 0, 0, 0, 12, 4, 8},
    {14, 17, 1, 13, 21, 21, 14}, {0, 4, 4, 31, 4, 4, 0}, {0, 1, 2, 4, 8, 16, 0}, {0, 12, 12, 0, 12, 12, 0},
};

static const uint8_t *glyph(char ch)
{
    const char *p = ch ? strchr(FONT_CHARS, ch) : NULL;
    if (!p) {
        p = strchr(FONT_CHARS, '?');
    }
    return FONT_ROWS[p - FONT_CHARS];
}

/* Capitals, single spaces, no leading or trailing space; other UTF-8 characters become '?' */
static void clean_text(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < size; p++) {
        if ((*p & 0xC0) == 0x80) {
            continue;
        }
        char c = *p >= 0x80 ? '?' : (char)toupper(*p);
        if (isspace((unsigned char)c)) {
            if (n == 0 || out[n - 1] == ' ') {
                continue;
            }
            c = ' ';
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

/* Splits text into exactly `lines` rows at spaces, about equally long; false if it can't */
static bool wrap(const char *text, int lines, char rows[3][APP_SETTINGS_NAME_MAX + 1])
{
    int len = (int)strlen(text);
    int target = (len + lines - 1) / lines;
    int count = 0;
    char cur[APP_SETTINGS_NAME_MAX + 1] = "";
    const char *p = text;
    while (*p) {
        const char *sp = strchr(p, ' ');
        int wlen = sp ? (int)(sp - p) : (int)strlen(p);
        int cur_len = (int)strlen(cur);
        int next_len = cur_len ? cur_len + 1 + wlen : wlen;
        if (cur_len && next_len > target && count < lines - 1) {
            strlcpy(rows[count++], cur, sizeof(rows[0]));
            snprintf(cur, sizeof(cur), "%.*s", wlen, p);
        } else if (cur_len) {
            size_t at = strlen(cur);
            snprintf(cur + at, sizeof(cur) - at, " %.*s", wlen, p);
        } else {
            snprintf(cur, sizeof(cur), "%.*s", wlen, p);
        }
        p = sp ? sp + 1 : p + wlen;
    }
    strlcpy(rows[count++], cur, sizeof(rows[0]));
    return count == lines;
}

static const text_set_t *s_sort_set;

static int cmp_dot_x(const void *a, const void *b)
{
    const pt_t *pa = &s_sort_set->pts[*(const int16_t *)a], *pb = &s_sort_set->pts[*(const int16_t *)b];
    if (pa->x != pb->x) {
        return pa->x < pb->x ? -1 : 1;
    }
    return pa->y < pb->y ? -1 : (pa->y > pb->y);
}

static bool layout_try(const char *text, text_set_t *set)
{
    for (int p = 9; p >= 4; p--) {
        for (int lines = 1; lines <= 3; lines++) {
            char rows[3][APP_SETTINGS_NAME_MAX + 1];
            if (!wrap(text, lines, rows)) {
                continue;
            }
            int n = SIZE / p;
            float mid = (n - 1) / 2.0f, lim = CENTRE - p * 0.6f;
            int max_len = 0;
            for (int i = 0; i < lines; i++) {
                int l = (int)strlen(rows[i]);
                max_len = l > max_len ? l : max_len;
            }
            int w = max_len * 6 - 1, h = lines * 7 + (lines - 1) * 3;
            int c0 = (int)roundf(mid - (w - 1) / 2.0f), r0 = (int)roundf(mid - (h - 1) / 2.0f);
            const int corners[4][2] = { { c0 - 1, r0 - 1 }, { c0 + w, r0 - 1 }, { c0 - 1, r0 + h }, { c0 + w, r0 + h } };
            bool fits = true;
            for (int k = 0; k < 4 && fits; k++) {
                fits = hypotf((corners[k][0] - mid) * p, (corners[k][1] - mid) * p) <= lim;
            }
            if (!fits) {
                continue;
            }

            set->n_pts = 0;
            set->rad = p * 0.36f;
            for (int i = 0; i < n * n; i++) {
                s_n.cell_map[i] = -1;
            }
            for (int li = 0; li < lines; li++) {
                int len = (int)strlen(rows[li]);
                int lc0 = (int)roundf(mid - (len * 6 - 2) / 2.0f);
                for (int ci = 0; ci < len; ci++) {
                    const uint8_t *g = glyph(rows[li][ci]);
                    for (int fr = 0; fr < 7; fr++) {
                        for (int fc = 0; fc < 5; fc++) {
                            if (!(g[fr] & (16 >> fc)) || set->n_pts >= MAX_PTS) {
                                continue;
                            }
                            int c = lc0 + ci * 6 + fc, r = r0 + li * 10 + fr;
                            if (c < 0 || c >= n || r < 0 || r >= n) {
                                continue;
                            }
                            s_n.cell_map[r * n + c] = (int16_t)set->n_pts;
                            set->pts[set->n_pts++] = (pt_t) { c, r, CENTRE + (c - mid) * p, CENTRE + (r - mid) * p };
                        }
                    }
                }
            }

            /* Stroke links: right and down neighbours, and a diagonal only where no corner joins them */
            set->n_links = 0;
            for (int i = 0; i < set->n_pts; i++) {
                int c = set->pts[i].c, r = set->pts[i].r;
#define AT(dc, dr) ((c + (dc)) >= 0 && (c + (dc)) < n && (r + (dr)) < n ? s_n.cell_map[(r + (dr)) * n + c + (dc)] : -1)
                int right = AT(1, 0), down = AT(0, 1);
                if (right >= 0) {
                    set->links[set->n_links][0] = i;
                    set->links[set->n_links++][1] = right;
                }
                if (down >= 0) {
                    set->links[set->n_links][0] = i;
                    set->links[set->n_links++][1] = down;
                }
                for (int dc = -1; dc <= 1; dc += 2) {
                    int diag = AT(dc, 1);
                    if (diag >= 0 && AT(dc, 0) < 0 && down < 0) {
                        set->links[set->n_links][0] = i;
                        set->links[set->n_links++][1] = diag;
                    }
                }
#undef AT
            }
            for (int i = 0; i < set->n_pts; i++) {
                set->order[i] = i;
            }
            s_sort_set = set;
            qsort(set->order, set->n_pts, sizeof(int16_t), cmp_dot_x);
            return true;
        }
    }
    return false;
}

static void layout(const char *text, text_set_t *set)
{
    if (!layout_try(text, set)) {
        char shorter[21];
        strlcpy(shorter, text, sizeof(shorter));
        clean_text(shorter, shorter, sizeof(shorter));
        if (!layout_try(shorter, set)) {
            set->n_pts = set->n_links = 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Building the network, and planning a play
 * ---------------------------------------------------------------------------------------------- */

/* RGB565, byte-swapped: the display runs with swap on, and images are copied into the buffer as-is */
static uint16_t to565(float r, float g, float b)
{
    uint8_t r8 = (uint8_t)(clamp01(r / 255.0f) * 255.0f + 0.5f);
    uint8_t g8 = (uint8_t)(clamp01(g / 255.0f) * 255.0f + 0.5f);
    uint8_t b8 = (uint8_t)(clamp01(b / 255.0f) * 255.0f + 0.5f);
    uint16_t v = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
    return (uint16_t)((v << 8) | (v >> 8));
}

static void make_lut(uint32_t rgb)
{
    float r = (float)((rgb >> 16) & 0xFF), g = (float)((rgb >> 8) & 0xFF), b = (float)(rgb & 0xFF);
    for (int i = 0; i < 256; i++) {
        float v = i / LEVEL;
        if (v <= 1.0f) {
            s_n.lut[i] = to565(r * v, g * v, b * v);
        } else {
            float t = clamp01((v - 1.0f) / 0.55f);
            s_n.lut[i] = to565(r + (255 - r) * t, g + (255 - g) * t, b + (255 - b) * t);
        }
    }
}

static void build(void)
{
    s_n.built_version = app_settings_version();
    s_n.built = true;
    make_lut(app_settings_get_neuron_color());

    /* The name first, then the Neurons texts; empty ones are skipped */
    char raw[APP_SETTINGS_NAME_MAX + 1], text[APP_SETTINGS_NAME_MAX + 1];
    s_n.n_texts = 0;
    for (int i = 0; i < MAX_TEXTS; i++) {
        if (i == 0) {
            app_settings_get_name(raw, sizeof(raw));
        } else {
            app_settings_get_text(i - 1, raw, sizeof(raw));
        }
        clean_text(raw, text, sizeof(text));
        if (text[0]) {
            layout(text, &s_n.sets[s_n.n_texts]);
            if (s_n.sets[s_n.n_texts].n_pts > 0) {
                s_n.n_texts++;
            }
        }
    }
    if (s_n.n_texts == 0) {
        layout("VOCAT", &s_n.sets[0]);
        s_n.n_texts = 1;
    }

    int pool = 0;
    for (int i = 0; i < s_n.n_texts; i++) {
        pool = s_n.sets[i].n_pts > pool ? s_n.sets[i].n_pts : pool;
    }
    int extra = (int)roundf(pool * 0.35f);
    s_n.total = pool + (extra > 24 ? extra : 24);
    if (s_n.total > MAX_NODES) {
        s_n.total = MAX_NODES;
    }

    /* Random resting places, spread out by best-candidate sampling */
    int tries = s_n.total > 400 ? 5 : 10;
    for (int i = 0; i < s_n.total; i++) {
        float bx = CENTRE, by = CENTRE, bd = -1.0f;
        for (int k = 0; k < tries; k++) {
            float a = rnd() * TAU, r = HOME_R * sqrtf(rnd());
            float x = CENTRE + cosf(a) * r, y = CENTRE + sinf(a) * r;
            float d = 1e9f;
            for (int j = 0; j < i; j++) {
                float dx = s_n.nodes[j].hx - x, dy = s_n.nodes[j].hy - y;
                float dd = dx * dx + dy * dy;
                d = dd < d ? dd : d;
            }
            if (d > bd) {
                bd = d;
                bx = x;
                by = y;
            }
        }
        s_n.nodes[i] = (node_t) {
            .hx = bx, .hy = by,
            .ax = 12.0f + rnd() * 24.0f, .ay = 12.0f + rnd() * 24.0f,
            .wx = 0.25f + rnd() * 0.4f, .wy = 0.25f + rnd() * 0.4f,
            .phx = rnd() * TAU, .phy = rnd() * TAU,
            .curve = (rnd() - 0.5f) * 70.0f,
            .x = bx, .y = by, .rad = 2.4f, .e = 1.0f, .last = -9.0f,
        };
    }
    float spacing = sqrtf(3.14159265f * HOME_R * HOME_R / s_n.total);
    s_n.near = spacing * 1.25f;
    s_n.far = spacing * 1.8f;
    s_n.n_pulses = 0;
    s_n.active = false;

    ESP_LOGI(TAG, "%d texts, %d nodes, colour #%06lX", s_n.n_texts, s_n.total,
             (unsigned long)app_settings_get_neuron_color());
}

static int cmp_key(const void *a, const void *b)
{
    float ka = ((const typeof(*s_n.sort_buf) *)a)->key, kb = ((const typeof(*s_n.sort_buf) *)b)->key;
    return ka < kb ? -1 : (ka > kb);
}

static void plan(void)
{
    int n = s_n.n_texts, total = s_n.total;
    s_n.T[0] = 0.0f;
    s_n.T[1] = TAP_LEAD_S;
    float hold = (float)app_settings_get_neuron_hold();
    for (int i = 2; i <= n + 1; i++) {
        s_n.T[i] = s_n.T[i - 1] + SPAN_S + vary(hold);
    }
    s_n.cycle = s_n.T[n + 1] + SPAN_S;
    s_n.n_kicks = 0;

    for (int i = 0; i < total; i++) {
        s_n.assign[0][i] = -1;
        s_n.delay[0][i] = 0.0f;
    }
    for (int j = 1; j <= n; j++) {
        const text_set_t *set = &s_n.sets[j - 1];
        const int16_t *prev = s_n.assign[j - 1];
        const text_set_t *prev_set = j >= 2 ? &s_n.sets[j - 2] : NULL;
        for (int i = 0; i < total; i++) {
            s_n.pos_x[i] = prev[i] >= 0 ? prev_set->pts[prev[i]].x : s_n.nodes[i].hx;
            s_n.pos_y[i] = prev[i] >= 0 ? prev_set->pts[prev[i]].y : s_n.nodes[i].hy;
        }

        /* Nodes already in letters morph first; the rest come from roughly nearest the centre */
        for (int i = 0; i < total; i++) {
            s_n.sort_buf[i].idx = i;
            s_n.sort_buf[i].key = (prev[i] >= 0 ? 0.0f : 1000.0f) +
                                  hypotf(s_n.pos_x[i] - CENTRE, s_n.pos_y[i] - CENTRE) + rnd() * 60.0f;
        }
        qsort(s_n.sort_buf, total, sizeof(s_n.sort_buf[0]), cmp_key);
        int m = set->n_pts < total ? set->n_pts : total;
        /* ... then pair them with the dots, both from left to right */
        for (int k = 0; k < m; k++) {
            int i = s_n.sort_buf[k].idx;
            s_n.sort_buf[k].key = s_n.pos_x[i] + s_n.pos_y[i] * 1e-4f;
        }
        qsort(s_n.sort_buf, m, sizeof(s_n.sort_buf[0]), cmp_key);

        int16_t *cur = s_n.assign[j];
        for (int i = 0; i < total; i++) {
            cur[i] = -1;
        }
        for (int k = 0; k < set->n_pts; k++) {
            s_n.node_of[j][k] = -1;
        }
        for (int k = 0; k < m; k++) {
            int dot = set->order[k], node = s_n.sort_buf[k].idx;
            cur[node] = dot;
            s_n.node_of[j][dot] = node;
        }

        float min_x = set->pts[set->order[0]].x, span_x = set->pts[set->order[set->n_pts - 1]].x - min_x;
        span_x = span_x < 1.0f ? 1.0f : span_x;
        for (int i = 0; i < total; i++) {
            s_n.delay[j][i] = cur[i] >= 0 ? (set->pts[cur[i]].x - min_x) / span_x * 0.5f + rnd() * 0.15f : rnd() * 0.4f;
        }

        /* 1 or 2 cascades from random letter dots at random moments of the hold */
        float hold_len = s_n.T[j + 1] - s_n.T[j] - SPAN_S;
        int count = 1 + (int)(rnd() * 2.0f);
        for (int k = 0; k < count && s_n.n_kicks < MAX_KICKS && m > 0; k++) {
            int node = s_n.node_of[j][set->order[(int)(rnd() * m) % m]];
            s_n.kicks[s_n.n_kicks++] = (kick_t) {
                .at = s_n.T[j] + SPAN_S + 0.1f + rnd() * fmaxf(0.1f, hold_len - 0.6f), .node = (int16_t)node,
            };
        }
    }
    for (int i = 0; i < total; i++) {
        s_n.assign[n + 1][i] = -1;
        s_n.delay[n + 1][i] = rnd() * 0.5f;
    }
}

/* ------------------------------------------------------------------------------------------------
 * Motion and firing
 * ---------------------------------------------------------------------------------------------- */

static void free_pos(const node_t *nd, float *x, float *y)
{
    float fx = nd->hx + nd->ax * sinf(nd->phx), fy = nd->hy + nd->ay * sinf(nd->phy);
    float dx = fx - CENTRE, dy = fy - CENTRE, d = sqrtf(dx * dx + dy * dy);
    if (d > FREE_R) {
        fx = CENTRE + dx / d * FREE_R;
        fy = CENTRE + dy / d * FREE_R;
    }
    *x = fx;
    *y = fy;
}

static void add_edge(int a, int b, float alpha, bool let)
{
    if (s_n.n_edges < MAX_EDGES && a >= 0 && b >= 0) {
        s_n.edges[s_n.n_edges++] = (edge_t) { (uint16_t)a, (uint16_t)b, alpha, let };
    }
}

static bool is_text(int phase)
{
    return phase >= 1 && phase <= s_n.n_texts;
}

/* Places every node for time tc into the play (0 = not playing), then wires them */
static void place(float tc)
{
    int n = s_n.n_texts, total = s_n.total;
    int j = 0;
    if (s_n.active) {
        for (int i = 0; i <= n + 1; i++) {
            if (s_n.T[i] <= tc) {
                j = i;
            }
        }
    }
    int prev = j == 0 ? n + 1 : j - 1;
    float step = ease(clamp01((tc - s_n.T[j]) / SPAN_S));
    s_n.shown_amount = (is_text(prev) ? 1.0f : 0.0f) + ((is_text(j) ? 1.0f : 0.0f) - (is_text(prev) ? 1.0f : 0.0f)) * step;
    if (!s_n.active) {
        s_n.shown_amount = 0.0f;
    }

    const text_set_t *set_a = is_text(prev) && s_n.active ? &s_n.sets[prev - 1] : NULL;
    const text_set_t *set_b = is_text(j) ? &s_n.sets[j - 1] : NULL;
    for (int i = 0; i < total; i++) {
        node_t *nd = &s_n.nodes[i];
        int a = set_a ? s_n.assign[prev][i] : -1, b = set_b ? s_n.assign[j][i] : -1;
        float ax, ay, bx, by, arad = 2.4f, brad = 2.4f, am = 0.0f, bm = 0.0f;
        if (a >= 0) {
            ax = set_a->pts[a].x;
            ay = set_a->pts[a].y;
            arad = set_a->rad;
            am = 1.0f;
        } else {
            free_pos(nd, &ax, &ay);
        }
        if (b >= 0) {
            bx = set_b->pts[b].x + sinf(s_n.wobble + nd->phx) * 0.8f;
            by = set_b->pts[b].y + cosf(s_n.wobble * 0.76f + nd->phy) * 0.8f;
            brad = set_b->rad;
            bm = 1.0f;
        } else {
            free_pos(nd, &bx, &by);
        }
        float e = j == 0 ? 1.0f : ease(clamp01((tc - s_n.T[j] - s_n.delay[j][i]) / MOVE_S));
        float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
        len = len < 1e-3f ? 1e-3f : len;
        float bend = (a < 0 && b < 0) ? 0.0f : sinf(3.14159265f * e) * nd->curve * fminf(1.0f, len / 60.0f);
        nd->x = ax + dx * e - dy / len * bend;
        nd->y = ay + dy * e + dx / len * bend;
        nd->rad = arad + (brad - arad) * e;
        nd->m = am + (bm - am) * e;
        nd->e = e;
    }

    /* Letter strokes: the new text's join up as their nodes arrive, the old text's fade as they leave */
    s_n.n_edges = 0;
    if (set_b) {
        for (int k = 0; k < set_b->n_links; k++) {
            int na = s_n.node_of[j][set_b->links[k][0]], nb = s_n.node_of[j][set_b->links[k][1]];
            if (na >= 0 && nb >= 0) {
                add_edge(na, nb, powf(fminf(s_n.nodes[na].e, s_n.nodes[nb].e), 3.0f), true);
            }
        }
    }
    if (set_a && j != 0) {
        for (int k = 0; k < set_a->n_links; k++) {
            int na = s_n.node_of[prev][set_a->links[k][0]], nb = s_n.node_of[prev][set_a->links[k][1]];
            if (na >= 0 && nb >= 0) {
                add_edge(na, nb, powf(1.0f - fmaxf(s_n.nodes[na].e, s_n.nodes[nb].e), 3.0f), true);
            }
        }
    }

    /* Network wires between free nodes that are close, found through a grid of FAR-sized cells */
    float far = s_n.far, near = s_n.near;
    int g = (int)(SIZE / far) + 1;
    g = g > GRID_MAX ? GRID_MAX : g;
    for (int i = 0; i < g * g; i++) {
        s_n.grid_head[i] = -1;
    }
#define CELL(v) ((int)((v) / far) < 0 ? 0 : ((int)((v) / far) >= g ? g - 1 : (int)((v) / far)))
    for (int i = 0; i < total; i++) {
        if (s_n.nodes[i].m < 0.95f) {
            int cell = CELL(s_n.nodes[i].y) * g + CELL(s_n.nodes[i].x);
            s_n.grid_next[i] = s_n.grid_head[cell];
            s_n.grid_head[cell] = (int16_t)i;
        }
    }
    float dim = 1.0f - 0.6f * s_n.shown_amount;
    for (int p = 0; p < total; p++) {
        const node_t *np = &s_n.nodes[p];
        float fp = 1.0f - np->m;
        if (fp < 0.05f) {
            continue;
        }
        int cx = CELL(np->x), cy = CELL(np->y);
        for (int gy = cy - 1; gy <= cy + 1; gy++) {
            for (int gx = cx - 1; gx <= cx + 1; gx++) {
                if (gx < 0 || gy < 0 || gx >= g || gy >= g) {
                    continue;
                }
                for (int q = s_n.grid_head[gy * g + gx]; q >= 0; q = s_n.grid_next[q]) {
                    if (q <= p) {
                        continue;
                    }
                    const node_t *nq = &s_n.nodes[q];
                    float fq = 1.0f - nq->m;
                    float dx = nq->x - np->x, dy = nq->y - np->y;
                    float d2 = dx * dx + dy * dy;
                    if (fq < 0.05f || d2 > far * far) {
                        continue;
                    }
                    float d = sqrtf(d2);
                    float fade = d < near ? 1.0f : 1.0f - (d - near) / (far - near);
                    add_edge(p, q, fp * fq * fade * dim, false);
                }
            }
        }
    }
#undef CELL

    /* Wires strong enough to carry a signal, per node */
    for (int i = 0; i < total; i++) {
        s_n.nodes[i].adj_count = 0;
    }
    for (int k = 0; k < s_n.n_edges; k++) {
        if (s_n.edges[k].alpha > 0.3f) {
            s_n.nodes[s_n.edges[k].a].adj_count++;
            s_n.nodes[s_n.edges[k].b].adj_count++;
        }
    }
    uint16_t at = 0;
    for (int i = 0; i < total; i++) {
        s_n.nodes[i].adj_start = at;
        at += s_n.nodes[i].adj_count;
        s_n.nodes[i].adj_count = 0;
    }
    for (int k = 0; k < s_n.n_edges; k++) {
        const edge_t *e = &s_n.edges[k];
        if (e->alpha > 0.3f) {
            node_t *na = &s_n.nodes[e->a], *nb = &s_n.nodes[e->b];
            s_n.adj[na->adj_start + na->adj_count++] = (uint16_t)k;
            s_n.adj[nb->adj_start + nb->adj_count++] = (uint16_t)k;
        }
    }
}

static void fire(int i)
{
    if (i < 0 || i >= s_n.total) {
        return;
    }
    node_t *nd = &s_n.nodes[i];
    if (s_n.clock - nd->last < REFRACTORY_S) {
        return;
    }
    nd->last = s_n.clock;
    nd->flash = 1.0f;
    for (int k = 0; k < nd->adj_count; k++) {
        const edge_t *e = &s_n.edges[s_n.adj[nd->adj_start + k]];
        if (rnd() > (e->let ? PASS_LETTER : PASS_NET) || s_n.n_pulses >= MAX_PULSES) {
            continue;
        }
        int to = e->a == i ? e->b : e->a;
        const node_t *o = &s_n.nodes[to];
        float d = hypotf(o->x - nd->x, o->y - nd->y);
        s_n.pulses[s_n.n_pulses++] = (pulse_t) {
            .from = (uint16_t)i, .to = (uint16_t)to, .let = e->let, .t0 = s_n.clock,
            .dur = fmaxf(0.05f, d / (e->let ? 160.0f : 200.0f)),
        };
    }
}

/* ------------------------------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------------------------------- */

static inline void add_level(int x, int y, float v)
{
    if ((unsigned)x >= SIZE || (unsigned)y >= SIZE || v <= 0.0f) {
        return;
    }
    uint8_t *p = &s_n.level[y * SIZE + x];
    int sum = *p + (int)(v + 0.5f);
    *p = (uint8_t)(sum > 255 ? 255 : sum);
}

static inline float fpart(float v)
{
    return v - floorf(v);
}

/* Anti-aliased line (Xiaolin Wu) adding `level` */
static void draw_line(float x0, float y0, float x1, float y1, float level)
{
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float grad = dx < 1e-4f ? 1.0f : dy / dx;
    int xs = (int)roundf(x0), xe = (int)roundf(x1);
    float y = y0 + grad * (xs - x0);
    for (int x = xs; x <= xe; x++) {
        int yi = (int)floorf(y);
        float f = y - yi;
        if (steep) {
            add_level(yi, x, level * (1.0f - f));
            add_level(yi + 1, x, level * f);
        } else {
            add_level(x, yi, level * (1.0f - f));
            add_level(x, yi + 1, level * f);
        }
        y += grad;
    }
}

static const uint8_t *stamp_for(float r, int *half)
{
    int q = (int)(r * 4.0f + 0.5f);
    q = q < 1 ? 1 : (q > STAMP_Q_MAX ? STAMP_Q_MAX : q);
    if (!s_stamp.half[q]) {
        float rad = q / 4.0f;
        int h = (int)ceilf(rad) + 1, size = 2 * h + 1;
        if (s_stamp.used + size * size > STAMP_BYTES) {
            return NULL;
        }
        uint8_t *d = &s_stamp.data[s_stamp.used];
        for (int j = 0; j < size; j++) {
            for (int i = 0; i < size; i++) {
                float dx = (float)(i - h), dy = (float)(j - h);
                d[j * size + i] = (uint8_t)(clamp01(rad - sqrtf(dx * dx + dy * dy) + 0.5f) * 255.0f + 0.5f);
            }
        }
        s_stamp.off[q] = (uint16_t)s_stamp.used;
        s_stamp.half[q] = (uint8_t)h;
        s_stamp.used += size * size;
    }
    *half = s_stamp.half[q];
    return &s_stamp.data[s_stamp.off[q]];
}

/* Soft-edged disc adding `level`, centred on the pixel under (cx, cy) */
static void draw_disc(float cx, float cy, float r, float level)
{
    int h;
    const uint8_t *st = stamp_for(r, &h);
    int lv = (int)(level + 0.5f);
    if (!st || lv <= 0) {
        return;
    }
    lv = lv > 255 ? 255 : lv;
    int size = 2 * h + 1, x0 = (int)cx - h, y0 = (int)cy - h;
    for (int j = 0; j < size; j++) {
        int y = y0 + j;
        if ((unsigned)y >= SIZE) {
            continue;
        }
        uint8_t *row = &s_n.level[y * SIZE];
        const uint8_t *srow = &st[j * size];
        for (int i = 0; i < size; i++) {
            int x = x0 + i;
            if ((unsigned)x >= SIZE || !srow[i]) {
                continue;
            }
            int sum = row[x] + ((srow[i] * lv + 128) >> 8);
            row[x] = (uint8_t)(sum > 255 ? 255 : sum);
        }
    }
}

static void render(void)
{
    for (int y = 0; y < SIZE; y++) {
        if (s_n.span_x1[y] > s_n.span_x0[y]) {
            memset(&s_n.level[y * SIZE + s_n.span_x0[y]], 0, s_n.span_x1[y] - s_n.span_x0[y]);
        }
    }

    for (int k = 0; k < s_n.n_edges; k++) {
        const edge_t *e = &s_n.edges[k];
        if (e->alpha < 0.02f) {
            continue;
        }
        const node_t *a = &s_n.nodes[e->a], *b = &s_n.nodes[e->b];
        float lit = fmaxf(a->flash, b->flash) * 0.2f;
        float v = e->alpha * ((e->let ? 0.55f : 0.22f) + lit) * (e->let ? 1.4f : 1.0f);
        draw_line(a->x, a->y, b->x, b->y, fminf(v, 1.0f) * LEVEL);
    }
    for (int k = 0; k < s_n.n_pulses; k++) {
        const pulse_t *p = &s_n.pulses[k];
        const node_t *a = &s_n.nodes[p->from], *b = &s_n.nodes[p->to];
        float u = clamp01((s_n.clock - p->t0) / p->dur), u0 = fmaxf(0.0f, u - 0.25f);
        float hx = a->x + (b->x - a->x) * u, hy = a->y + (b->y - a->y) * u;
        draw_line(a->x + (b->x - a->x) * u0, a->y + (b->y - a->y) * u0, hx, hy, 0.7f * LEVEL);
        draw_disc(hx, hy, 1.4f, 190.0f);
    }
    float free_base = 0.45f * (1.0f - 0.65f * s_n.shown_amount);
    for (int i = 0; i < s_n.total; i++) {
        const node_t *nd = &s_n.nodes[i];
        float base = free_base + (0.9f - free_base) * nd->m;
        float a = base + nd->flash * (0.35f + 0.2f * nd->m);
        if (a > 0.6f) {             /* the glow of a dim drifting node wouldn't show */
            draw_disc(nd->x, nd->y, nd->rad * 2.4f, fminf(a * 0.2f, 1.0f) * LEVEL);
        }
        draw_disc(nd->x, nd->y, nd->rad, fminf(a, 1.0f) * LEVEL);
        if (nd->flash > 0.5f) {
            draw_disc(nd->x, nd->y, nd->rad * 0.6f, (nd->flash - 0.5f) * 0.8f * (255.0f - LEVEL));
        }
    }

}

/* Level buffer to screen colours, inside the round screen only (the corners stay black), four pixels
 * at a time: the spans are whole words, and word accesses make the most of the PSRAM bus
 */
static void convert(uint16_t *pixels)
{
    const uint16_t *lut = s_n.lut;
    for (int y = 0; y < SIZE; y++) {
        const uint32_t *src = (const uint32_t *)&s_n.level[y * SIZE + s_n.span_x0[y]];
        uint32_t *dst = (uint32_t *)&pixels[y * SIZE + s_n.span_x0[y]];
        for (int n = (s_n.span_x1[y] - s_n.span_x0[y]) / 4; n > 0; n--) {
            uint32_t v = *src++;
            dst[0] = lut[v & 0xFF] | ((uint32_t)lut[(v >> 8) & 0xFF] << 16);
            dst[1] = lut[(v >> 16) & 0xFF] | ((uint32_t)lut[v >> 24] << 16);
            dst += 2;
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Frames: drawn in neurons_task, shown by the gfx task
 * ---------------------------------------------------------------------------------------------- */

/* Advances the network to now and draws it into `pixels` */
static void draw_frame(uint16_t *pixels)
{
    int64_t now = esp_timer_get_time();
    if (s_n.seen_show_count != s_n.show_count) {
        /* Shown again: start at rest, with no signals in flight */
        s_n.seen_show_count = s_n.show_count;
        s_n.active = false;
        s_n.n_pulses = 0;
        s_n.last_us = now;
    }
    float dt = fminf(0.05f, (float)(now - s_n.last_us) / 1e6f);
    dt = dt < 0.0f ? 0.0f : dt;
    s_n.last_us = now;
    if (!s_n.active && (!s_n.built || s_n.built_version != app_settings_version())) {
        build();
    }
    if (s_n.pending_tap) {
        s_n.pending_tap = false;
        if (!s_n.active) {
            plan();
            s_n.active = true;
            s_n.start_us = now;
            s_n.last_tc = 0.0f;
            ESP_LOGI(TAG, "Playing %d texts, %.1f s", s_n.n_texts, s_n.cycle);
        }
    }
    float tc = s_n.active ? (float)(now - s_n.start_us) / 1e6f : 0.0f;
    if (s_n.active && tc >= s_n.cycle) {
        s_n.active = false;
        tc = 0.0f;
    }
    float prev_tc = s_n.last_tc;
    s_n.last_tc = tc;

    /* Drift phases advance by themselves, so the motion stays smooth however long the page is up */
    for (int i = 0; i < s_n.total; i++) {
        node_t *nd = &s_n.nodes[i];
        nd->phx = fmodf(nd->phx + nd->wx * dt, TAU);
        nd->phy = fmodf(nd->phy + nd->wy * dt, TAU);
    }
    s_n.wobble = fmodf(s_n.wobble + 1.7f * dt, TAU * 100.0f);
    s_n.clock += dt;
    if (s_n.clock > 1000.0f) {      /* keep the clock small enough for float precision */
        s_n.clock -= 1000.0f;
        for (int k = 0; k < s_n.n_pulses; k++) {
            s_n.pulses[k].t0 -= 1000.0f;
        }
        for (int i = 0; i < s_n.total; i++) {
            s_n.nodes[i].last -= 1000.0f;
        }
    }

    int64_t t_place = esp_timer_get_time();
    place(tc);
    s_n.time_place_us += esp_timer_get_time() - t_place;
    if (rnd() < dt * SPARKS_PER_S) {
        fire((int)(rnd() * s_n.total) % s_n.total);
    }
    if (s_n.active) {
        for (int k = 0; k < s_n.n_kicks; k++) {
            if (prev_tc < s_n.kicks[k].at && tc >= s_n.kicks[k].at) {
                fire(s_n.kicks[k].node);
            }
        }
    }
    int kept = 0;
    int n_before = s_n.n_pulses;
    for (int k = 0; k < n_before; k++) {
        pulse_t p = s_n.pulses[k];
        if (s_n.clock >= p.t0 + p.dur) {
            fire(p.to);     /* may append new pulses after n_before */
        } else {
            s_n.pulses[kept++] = p;
        }
    }
    int added = s_n.n_pulses - n_before;
    memmove(&s_n.pulses[kept], &s_n.pulses[n_before], added * sizeof(pulse_t));
    s_n.n_pulses = kept + added;
    float decay = expf(-dt * 3.5f);
    for (int i = 0; i < s_n.total; i++) {
        s_n.nodes[i].flash *= decay;
    }

    int64_t t_draw = esp_timer_get_time();
    render();
    int64_t t_convert = esp_timer_get_time();
    convert(pixels);
    s_n.time_draw_us += t_convert - t_draw;
    s_n.time_convert_us += esp_timer_get_time() - t_convert;

    if (s_n.frames == 0) {
        s_n.first_frame_us = now;
    }
    if (++s_n.frames == 150) {
        ESP_LOGI(TAG, "Per frame: place %d ms, draw %d ms, convert %d ms (level buffer in %s RAM); %d ms apart; "
                 "%d edges, %d signals", (int)(s_n.time_place_us / s_n.frames / 1000),
                 (int)(s_n.time_draw_us / s_n.frames / 1000), (int)(s_n.time_convert_us / s_n.frames / 1000),
                 s_n.level_internal ? "internal" : "PSRAM",
                 (int)((now - s_n.first_frame_us) / (s_n.frames - 1) / 1000), s_n.n_edges, s_n.n_pulses);
        s_n.frames = 0;
        s_n.time_place_us = s_n.time_draw_us = s_n.time_convert_us = 0;
    }
}

/* Draws frames while the page is shown, into whichever image is neither on screen nor waiting */
static void neurons_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_n.shown) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t started = esp_timer_get_time();
        portENTER_CRITICAL(&s_buf_lock);
        int target = 0;
        while (target == s_n.on_screen || target == s_n.ready) {
            target++;
        }
        portEXIT_CRITICAL(&s_buf_lock);

        draw_frame(s_n.pixels[target]);

        portENTER_CRITICAL(&s_buf_lock);
        s_n.ready = target;         /* replaces an older finished image the gfx task hasn't shown */
        portEXIT_CRITICAL(&s_buf_lock);
        /* Always leave a gap, so lower-priority tasks on this core get to run */
        int wait_ms = MIN_FRAME_MS - (int)((esp_timer_get_time() - started) / 1000);
        vTaskDelay(pdMS_TO_TICKS(wait_ms < 3 ? 3 : wait_ms));
    }
}

/* gfx task: show the newest finished image */
static void frame_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_buf_lock);
    int ready = s_n.ready;
    if (ready >= 0) {
        s_n.on_screen = ready;
        s_n.ready = -1;
    }
    portEXIT_CRITICAL(&s_buf_lock);
    if (ready >= 0) {
        gfx_img_set_src(s_n.img, &s_n.dsc[ready]);     /* also marks the image for redrawing */
    }
}

/* ------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------- */

esp_err_t app_neurons_create(void *gfx_handle)
{
    ESP_RETURN_ON_FALSE(gfx_handle, ESP_ERR_INVALID_ARG, TAG, "No gfx handle");
    ESP_RETURN_ON_FALSE(s_n.gfx == NULL, ESP_ERR_INVALID_STATE, TAG, "Already created");
    s_n.gfx = (gfx_handle_t)gfx_handle;

    for (int i = 0; i < N_IMAGES; i++) {
        s_n.pixels[i] = alloc_psram(SIZE * SIZE * sizeof(uint16_t));
    }
    /* The level buffer is the busiest memory: internal RAM if that still leaves plenty for Wi-Fi */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > SIZE * SIZE + 96 * 1024) {
        s_n.level = heap_caps_calloc(1, SIZE * SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_n.level_internal = s_n.level != NULL;
    if (!s_n.level) {
        s_n.level = alloc_psram(SIZE * SIZE);
    }
    s_n.nodes = alloc_psram(MAX_NODES * sizeof(node_t));
    s_n.edges = alloc_psram(MAX_EDGES * sizeof(edge_t));
    s_n.adj = alloc_psram(2 * MAX_EDGES * sizeof(uint16_t));
    s_n.grid_next = alloc_psram(MAX_NODES * sizeof(int16_t));
    s_n.cell_map = alloc_psram((SIZE / 4) * (SIZE / 4) * sizeof(int16_t));
    s_n.sort_buf = alloc_psram(MAX_NODES * sizeof(s_n.sort_buf[0]));
    s_n.pos_x = alloc_psram(MAX_NODES * sizeof(float));
    s_n.pos_y = alloc_psram(MAX_NODES * sizeof(float));
    bool ok = s_n.pixels[0] && s_n.pixels[1] && s_n.pixels[2] && s_n.level && s_n.nodes && s_n.edges && s_n.adj && s_n.grid_next && s_n.cell_map &&
              s_n.sort_buf && s_n.pos_x && s_n.pos_y;
    for (int i = 0; i < MAX_TEXTS && ok; i++) {
        s_n.sets[i].pts = alloc_psram(MAX_PTS * sizeof(pt_t));
        s_n.sets[i].links = alloc_psram(3 * MAX_PTS * sizeof(s_n.sets[i].links[0]));     /* right, down, or two diagonals */
        s_n.sets[i].order = alloc_psram(MAX_PTS * sizeof(int16_t));
        ok = s_n.sets[i].pts && s_n.sets[i].links && s_n.sets[i].order;
    }
    for (int j = 0; j < MAX_PHASES && ok; j++) {
        s_n.assign[j] = alloc_psram(MAX_NODES * sizeof(int16_t));
        s_n.delay[j] = alloc_psram(MAX_NODES * sizeof(float));
        s_n.node_of[j] = alloc_psram(MAX_PTS * sizeof(int16_t));
        ok = s_n.assign[j] && s_n.delay[j] && s_n.node_of[j];
    }
    ESP_RETURN_ON_FALSE(ok, ESP_ERR_NO_MEM, TAG, "Out of memory");

    for (int y = 0; y < SIZE; y++) {
        float dy = y + 0.5f - CENTRE, half = CENTRE * CENTRE - dy * dy > 0 ? sqrtf(CENTRE * CENTRE - dy * dy) : 0.0f;
        int x0 = (int)floorf(CENTRE - half), x1 = (int)ceilf(CENTRE + half);
        x0 = (x0 < 0 ? 0 : x0) & ~3;            /* whole words, for convert() */
        x1 = ((x1 > SIZE ? SIZE : x1) + 3) & ~3;
        s_n.span_x0[y] = (int16_t)x0;
        s_n.span_x1[y] = (int16_t)(x1 > SIZE ? SIZE : x1);
    }

    for (int i = 0; i < N_IMAGES; i++) {
        s_n.dsc[i].header.magic = C_ARRAY_HEADER_MAGIC;
        s_n.dsc[i].header.cf = GFX_COLOR_FORMAT_RGB565;
        s_n.dsc[i].header.w = SIZE;
        s_n.dsc[i].header.h = SIZE;
        s_n.dsc[i].header.stride = SIZE * 2;
        s_n.dsc[i].data_size = SIZE * SIZE * 2;
        s_n.dsc[i].data = (const uint8_t *)s_n.pixels[i];
    }
    s_n.ready = -1;
    s_n.on_screen = 0;

    s_n.img = gfx_img_create(s_n.gfx);
    ESP_RETURN_ON_FALSE(s_n.img, ESP_ERR_NO_MEM, TAG, "Failed to create the image");
    gfx_img_set_src(s_n.img, &s_n.dsc[0]);
    gfx_obj_set_pos(s_n.img, 0, 0);
    gfx_obj_set_visible(s_n.img, false);

    s_n.timer = gfx_timer_create(s_n.gfx, frame_cb, FRAME_MS, NULL);
    ESP_RETURN_ON_FALSE(s_n.timer, ESP_ERR_NO_MEM, TAG, "Failed to create the frame timer");
    gfx_timer_pause(s_n.timer);
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(neurons_task, "neurons", 6144, NULL, 2, &s_n.task, TASK_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create the task");

    ESP_LOGI(TAG, "Neurons page ready (level buffer in %s RAM), %u KB PSRAM and %u KB internal RAM left",
             s_n.level_internal ? "internal" : "PSRAM", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return ESP_OK;
}

bool app_neurons_show(bool show)
{
    if (!s_n.img) {
        return false;
    }
    if (show == s_n.shown) {
        return show;
    }
    s_n.shown = show;
    if (show) {
        /* Shows the last image until the task has drawn a fresh one (black the first time) */
        s_n.show_count++;
        gfx_obj_set_visible(s_n.img, true);
        app_display_set_face_visible(false);
        gfx_timer_resume(s_n.timer);
        xTaskNotifyGive(s_n.task);
    } else {
        gfx_timer_pause(s_n.timer);
        gfx_obj_set_visible(s_n.img, false);
        app_display_set_face_visible(true);
        s_n.pending_tap = false;    /* the task ends a play when the page is shown again */
    }
    return show;
}

void app_neurons_tap(void)
{
    if (s_n.shown && !s_n.active) {
        s_n.pending_tap = true;
    }
}
