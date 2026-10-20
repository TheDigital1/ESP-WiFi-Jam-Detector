/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Spectrum screen, 480x320 landscape:
 *   header  : band and its detector state (coloured); the other band's state
 *   plot    : drawn by plot.c straight to the panel (not through LVGL):
 *             current average (cyan, filled), max-hold (orange) and learned
 *             baseline (grey), relative dB
 *   axis    : band edges in MHz, Wi-Fi channel ticks, U-NII labels
 *   footer  : gain / timing / clipping, view toggle (spectrum / waterfall),
 *             "2.4 / 5" band toggle and "Reset max" buttons
 * Waterfall view: one row per completed sweep, newest at the top, coloured by
 * dB above the learned baseline (absolute level while learning). History is
 * kept for both bands all the time, so it is there when the view opens.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "board.h"
#include "detector.h"
#include "plot.h"
#include "sweep.h"
#include "ui_priv.h"

static const char *TAG = "ui_spectrum";

#define PLOT_X        28
#define PLOT_Y        26
#define PLOT_W        (BOARD_LCD_W - PLOT_X - 2)
#define PLOT_H        236
#define Y_MIN_CDB     0
#define Y_MAX_CDB     9000
#define STRIP_ROWS    12 /* plot strip: 450 x 12 px = 10.8 KB; two internal DMA buffers */
#define WF_LUT        64
#define WF_REL_LO     (-500) /* waterfall colour range, cdB above baseline */
#define WF_REL_HI     3000
#define WF_ABS_LO     1000   /* ... while learning, absolute cdB */
#define WF_ABS_HI     6000
#define WF_LABELS     3
#define AXIS_GAP      3  /* px between axis labels in a row */

/* Waterfall history, newest row at head; big-endian RGB565, PSRAM (2 x 212 KB). */
typedef struct {
    uint16_t *px;            /* PLOT_H rows of PLOT_W */
    uint32_t t_ms[PLOT_H];
    unsigned head, count;
    uint32_t sweeps;         /* sweep count of the newest row */
} waterfall_t;

/* The plot area is not an LVGL object: LVGL only paints it once (screen
 * background) and never invalidates it again. A full LVGL redraw of this
 * area cost ~60 ms of CPU per frame on this single core; drawing it directly
 * costs a fraction of that. */
static lv_obj_t *s_scr, *s_header, *s_rates, *s_footer, *s_axis;
static uint16_t *s_strip[2];
static SemaphoreHandle_t s_strip_free[2];
/* Column data lives in PSRAM: internal SRAM is reserved for the capture
 * bank and Wi-Fi (see components/espsdr_rx). */
EXT_RAM_BSS_ATTR static int16_t s_col_avg[PLOT_W], s_col_hold[PLOT_W], s_col_base[PLOT_W];
EXT_RAM_BSS_ATTR static plot_col_t s_cols[PLOT_W];
static uint16_t s_grid_x[16];
static unsigned s_n_grid_x;
static band_id_t s_band = BAND_24;
static bool s_painted; /* LVGL has painted the static parts since the screen was loaded */
static sweep_view_t s_view;
static int16_t *s_base; /* detector baseline, n_bins */
typedef struct {
    lv_obj_t *obj;
    int16_t left, right;
    uint8_t row;
} axis_label_t;
static axis_label_t s_axis_labels[16];
static unsigned s_n_axis_labels;
static waterfall_t s_wf[BAND_COUNT];
static uint16_t s_lut[WF_LUT];
static bool s_waterfall, s_wf_dirty;
static lv_obj_t *s_db_labels[8], *s_t_labels[WF_LABELS], *s_view_label, *s_reset_btn;
static uint32_t s_now_ms;

/* Colours in the panel's big-endian order: strips go straight to the LCD. */
static const plot_style_t s_style = {
    .min_cdb = Y_MIN_CDB,
    .max_cdb = Y_MAX_CDB,
    .grid_step_cdb = 1000,
    .bg = PLOT_RGB565_BE(0x10, 0x14, 0x18),
    .grid = PLOT_RGB565_BE(0x2a, 0x30, 0x38),
    .fill = PLOT_RGB565_BE(0x0c, 0x3a, 0x4a),
    .avg = PLOT_RGB565_BE(0x35, 0xd0, 0xff),
    .hold = PLOT_RGB565_BE(0xff, 0x9a, 0x2e),
    .base = PLOT_RGB565_BE(0x70, 0x70, 0x70),
};

static void strip_done(void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    portYIELD_FROM_ISR(woken);
}

typedef void (*fill_fn_t)(void *ctx, unsigned y0, unsigned rows, uint16_t *px);

static void fill_plot(void *ctx, unsigned y0, unsigned rows, uint16_t *px)
{
    plot_rows((const plot_frame_t *)ctx, y0, rows, px, PLOT_W);
}

static void fill_waterfall(void *ctx, unsigned y0, unsigned rows, uint16_t *px)
{
    const waterfall_t *w = ctx;
    for (unsigned r = 0; r < rows; r++, px += PLOT_W) {
        const unsigned age = y0 + r;
        if (age < w->count) {
            memcpy(px, w->px + (size_t)((w->head + age) % PLOT_H) * PLOT_W, PLOT_W * sizeof(uint16_t));
        } else {
            for (unsigned x = 0; x < PLOT_W; x++) {
                px[x] = s_style.bg;
            }
        }
    }
}

/* Fill and send the plot area strip by strip; strip n+1 is rendered while
 * strip n is still being sent. */
static esp_err_t draw_area(fill_fn_t fill, void *ctx)
{
    esp_err_t err = ESP_OK;
    unsigned i = 0;
    for (unsigned y0 = 0; y0 < PLOT_H && err == ESP_OK; y0 += STRIP_ROWS, i ^= 1) {
        unsigned rows = y0 + STRIP_ROWS <= PLOT_H ? STRIP_ROWS : PLOT_H - y0;
        if (xSemaphoreTake(s_strip_free[i], pdMS_TO_TICKS(500)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        fill(ctx, y0, rows, s_strip[i]);
        err = board_lcd_draw_async(PLOT_X, PLOT_Y + y0, PLOT_X + PLOT_W, PLOT_Y + y0 + rows, s_strip[i], strip_done,
                                   s_strip_free[i]);
        if (err != ESP_OK) {
            xSemaphoreGive(s_strip_free[i]);
        }
    }
    /* Wait for both buffers before returning, so the next frame starts clean. */
    for (int k = 0; k < 2; k++) {
        if (xSemaphoreTake(s_strip_free[k], pdMS_TO_TICKS(500)) == pdTRUE) {
            xSemaphoreGive(s_strip_free[k]);
        } else if (err == ESP_OK) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    return err;
}

static esp_err_t draw_plot(void)
{
    static const plot_buf_t geom = {.w = PLOT_W, .h = PLOT_H};
    plot_frame_t f = {.geom = &geom, .st = &s_style, .cols = s_cols};
    plot_prepare(&f, s_col_avg, s_col_hold, s_col_base, s_grid_x, s_n_grid_x);
    return draw_area(fill_plot, &f);
}

/* Dark blue -> blue -> cyan -> yellow -> red. */
static void build_lut(void)
{
    static const uint8_t stops[][3] = {
        {0x10, 0x14, 0x30}, {0x10, 0x40, 0xc0}, {0x20, 0xd0, 0xe0}, {0xf0, 0xe0, 0x30}, {0xff, 0x30, 0x20},
    };
    const unsigned n = sizeof(stops) / sizeof(stops[0]);
    for (unsigned i = 0; i < WF_LUT; i++) {
        const unsigned pos = i * (n - 1) * 256 / (WF_LUT - 1), a = pos / 256, f = pos % 256;
        const unsigned b = a + 1 < n ? a + 1 : a;
        uint8_t c[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (uint8_t)((stops[a][k] * (256 - f) + stops[b][k] * f) / 256);
        }
        s_lut[i] = PLOT_RGB565_BE(c[0], c[1], c[2]);
    }
}

static uint16_t wf_colour(int32_t v, int32_t lo, int32_t hi)
{
    int32_t i = (v - lo) * (WF_LUT - 1) / (hi - lo);
    return s_lut[i < 0 ? 0 : (i >= WF_LUT ? WF_LUT - 1 : i)];
}

/* One row per new sweep, for both bands. Each column shows the strongest
 * bin it covers, so narrow signals stay visible on the 5 GHz band. */
void ui_spectrum_feed(const ui_snap_t *snap)
{
    s_now_ms = snap->now_ms;
    for (int b = 0; b < BAND_COUNT; b++) {
        waterfall_t *w = &s_wf[b];
        sweep_view_t meta = {0};
        if (snap->sweep != SWEEP_RUNNING || sweep_get_view((band_id_t)b, &meta) != ESP_OK || !meta.valid ||
            meta.sweeps == w->sweeps) {
            continue;
        }
        sweep_view_t v = {.avg_cdb = s_view.avg_cdb};
        sweep_get_view((band_id_t)b, &v);
        const bool rel = snap->st[b].state != DETECT_LEARNING && detector_get_baseline((band_id_t)b, s_base) == ESP_OK;
        w->head = (w->head + PLOT_H - 1) % PLOT_H;
        w->count += w->count < PLOT_H;
        w->t_ms[w->head] = snap->now_ms;
        w->sweeps = v.sweeps;
        uint16_t *row = w->px + (size_t)w->head * PLOT_W;
        for (unsigned x = 0; x < PLOT_W; x++) {
            unsigned lo = x * v.n_bins / PLOT_W, hi = (x + 1) * v.n_bins / PLOT_W;
            hi = hi > lo ? hi : lo + 1;
            int32_t peak = INT32_MIN;
            for (unsigned k = lo; k < hi; k++) {
                const int32_t e = v.avg_cdb[k] - (rel ? s_base[k] : 0);
                peak = e > peak ? e : peak;
            }
            row[x] = rel ? wf_colour(peak, WF_REL_LO, WF_REL_HI) : wf_colour(peak, WF_ABS_LO, WF_ABS_HI);
        }
        if (b == (int)s_band) {
            s_wf_dirty = true;
        }
    }
}

static void fmt_age(char *buf, size_t len, uint32_t ms)
{
    const unsigned s = ms / 1000;
    if (s < 100) {
        snprintf(buf, len, "%us", s);
    } else {
        snprintf(buf, len, "%um", (s + 30) / 60);
    }
}

static void update_time_labels(void)
{
    const waterfall_t *w = &s_wf[s_band];
    for (int i = 0; i < WF_LABELS; i++) {
        const unsigned age = (unsigned)i * (PLOT_H - 1) / (WF_LABELS - 1);
        char buf[12] = "";
        if (i == 0) {
            snprintf(buf, sizeof(buf), "now");
        } else if (age < w->count) {
            fmt_age(buf, sizeof(buf), s_now_ms - w->t_ms[(w->head + age) % PLOT_H]);
        }
        ui_set_text(s_t_labels[i], buf);
    }
}

static void set_mode(bool waterfall)
{
    s_waterfall = waterfall;
    for (unsigned i = 0; i < sizeof(s_db_labels) / sizeof(s_db_labels[0]); i++) {
        lv_obj_set_flag(s_db_labels[i], LV_OBJ_FLAG_HIDDEN, waterfall);
    }
    for (int i = 0; i < WF_LABELS; i++) {
        lv_obj_set_flag(s_t_labels[i], LV_OBJ_FLAG_HIDDEN, !waterfall);
    }
    lv_obj_set_flag(s_reset_btn, LV_OBJ_FLAG_HIDDEN, waterfall);
    lv_label_set_text(s_view_label, waterfall ? "Spectrum" : "Waterfall");
    s_wf_dirty = true;
}

static int32_t khz_to_col(const band_plan_t *p, uint32_t khz)
{
    return (int32_t)((int64_t)(khz - p->start_khz) * (PLOT_W - 1) / (p->stop_khz - p->start_khz));
}

/* Two label rows under the plot. A label that would overlap one already
 * placed tries the other row and is otherwise left out, so the order of the
 * calls in build_axis() is their priority. */
static void add_axis_label(const char *text, int32_t x, int32_t row)
{
    if (s_n_axis_labels >= sizeof(s_axis_labels) / sizeof(s_axis_labels[0])) {
        return;
    }
    lv_obj_t *l = lv_label_create(s_axis);
    lv_label_set_text(l, text);
    lv_obj_update_layout(l);
    const int32_t w = lv_obj_get_width(l);
    int32_t left = x - w / 2;
    left = left < 0 ? 0 : (left + w > BOARD_LCD_W ? BOARD_LCD_W - w : left);
    for (int32_t r = 0; r < 2; r++, row ^= 1) {
        bool clear = true;
        for (unsigned i = 0; i < s_n_axis_labels && clear; i++) {
            const axis_label_t *o = &s_axis_labels[i];
            clear = o->row != row || left + w + AXIS_GAP <= o->left || o->right + AXIS_GAP <= left;
        }
        if (clear) {
            lv_obj_set_pos(l, left, row * 16);
            s_axis_labels[s_n_axis_labels++] = (axis_label_t){l, (int16_t)left, (int16_t)(left + w), (uint8_t)row};
            return;
        }
    }
    lv_obj_delete(l);
}

static void build_axis(void)
{
    const band_plan_t *p = sweep_plan(s_band);
    for (unsigned i = 0; i < s_n_axis_labels; i++) {
        lv_obj_delete(s_axis_labels[i].obj);
    }
    s_n_axis_labels = 0;
    s_n_grid_x = 0;
    char buf[24];
    for (unsigned i = 0; i < p->n_ticks; i++) {
        int32_t col = khz_to_col(p, p->ticks[i].khz);
        snprintf(buf, sizeof(buf), "ch%s", p->ticks[i].name);
        add_axis_label(buf, PLOT_X + col, 0);
        if (s_n_grid_x < sizeof(s_grid_x) / sizeof(s_grid_x[0])) {
            s_grid_x[s_n_grid_x++] = (uint16_t)col; /* ticks are listed in frequency order */
        }
    }
    for (unsigned i = 0; i < p->n_labels; i++) {
        add_axis_label(p->labels[i].name, PLOT_X + khz_to_col(p, (p->labels[i].lo_khz + p->labels[i].hi_khz) / 2),
                       1);
    }
    /* Band edges last: on 5 GHz they would sit on ch36/ch165 and U-NII-1/3,
     * so they are dropped there and the channel numbers carry the scale. */
    snprintf(buf, sizeof(buf), "%u", (unsigned)(p->start_khz / 1000));
    add_axis_label(buf, PLOT_X, 0);
    snprintf(buf, sizeof(buf), "%u", (unsigned)((p->stop_khz + 500) / 1000));
    add_axis_label(buf, PLOT_X + PLOT_W, 0);
    if (!p->n_labels) {
        add_axis_label("MHz", BOARD_LCD_W / 2, 1);
    }
}

static const char *short_state(const detect_status_t *st)
{
    static const char *names[] = {"learning", "normal", "suspect", "INTERFERENCE"};
    return names[st->state];
}

void ui_spectrum_update(const ui_snap_t *snap)
{
    const band_plan_t *p = sweep_plan(s_band);
    if (!s_painted || sweep_get_view(s_band, &s_view) != ESP_OK) {
        return;
    }
    const detect_status_t *st = snap->st;
    const bool have_base = st[s_band].state != DETECT_LEARNING && detector_get_baseline(s_band, s_base) == ESP_OK;
    for (unsigned x = 0; x < PLOT_W && !s_waterfall; x++) {
        if (!s_view.valid) {
            s_col_avg[x] = s_col_hold[x] = s_col_base[x] = PLOT_NONE;
            continue;
        }
        unsigned lo = x * s_view.n_bins / PLOT_W, hi = (x + 1) * s_view.n_bins / PLOT_W;
        hi = hi > lo ? hi : lo + 1;
        int32_t sum = 0, base = 0, peak = INT16_MIN;
        for (unsigned k = lo; k < hi; k++) {
            sum += s_view.avg_cdb[k];
            base += have_base ? s_base[k] : 0;
            peak = s_view.hold_cdb[k] > peak ? s_view.hold_cdb[k] : peak;
        }
        s_col_avg[x] = (int16_t)(sum / (int32_t)(hi - lo));
        s_col_base[x] = have_base ? (int16_t)(base / (int32_t)(hi - lo)) : PLOT_NONE;
        s_col_hold[x] = (int16_t)peak;
    }
    static int64_t draw_us, window;
    static unsigned frames;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    if (!s_waterfall) {
        err = draw_plot();
        frames++;
    } else if (s_wf_dirty) {
        s_wf_dirty = false; /* only when a row arrives: ~0.5-2 redraws/s */
        err = draw_area(fill_waterfall, &s_wf[s_band]);
        update_time_labels();
        frames++;
    }
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "plot draw: %s", esp_err_to_name(err));
    }
    draw_us += t1 - t0;
    if (t1 - window >= 10000000 && frames) {
        ESP_LOGI(TAG, "%s %.1f frames/s, %.1f ms per frame (render + LCD)", s_waterfall ? "waterfall" : "spectrum",
                 frames * 1e6f / (t1 - window), draw_us / 1000.0f / frames);
        draw_us = 0;
        frames = 0;
        window = t1;
    }

    char text[96], state[64];
    const sweep_state_t sw = snap->sweep;
    if (sw == SWEEP_RUNNING) {
        ui_state_text(state, sizeof(state), &st[s_band]);
        snprintf(text, sizeof(text), "%s  %s", p->name, state);
    } else {
        snprintf(text, sizeof(text), "%s  %s", p->name, sw == SWEEP_FAILED ? "SWEEP FAILED" : "auto-ranging gain...");
    }
    const band_id_t other = s_band == BAND_24 ? BAND_5 : BAND_24;
    char rates[40];
    snprintf(rates, sizeof(rates), "%s: %s", sweep_plan(other)->name,
             sw == SWEEP_RUNNING ? short_state(&st[other]) : "-");
    ui_set_text(s_rates, rates);
    ui_set_colour(s_rates, sw == SWEEP_RUNNING ? ui_state_colour(st[other].state) : lv_color_white());
    /* "INTERFERENCE: pulsed wideband" at 20 px would run into the other
     * band's state: the header gets what is left and ends in dots. */
    lv_point_t sz;
    lv_text_get_size(&sz, rates, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t header_w = BOARD_LCD_W - 4 - 4 - sz.x - 10;
    if (lv_obj_get_style_width(s_header, 0) != header_w) {
        lv_obj_set_width(s_header, header_w);
    }
    ui_set_text(s_header, text);
    ui_set_colour(s_header, sw == SWEEP_RUNNING ? ui_state_colour(st[s_band].state) : lv_color_white());
    /* Sweep time rounded to 10 ms so the footer does not redraw every frame. */
    snprintf(text, sizeof(text), "gain %u  %u ms  clip %u/%u", s_view.gain,
             (unsigned)(s_view.last_sweep_ms + 5) / 10 * 10, s_view.clipped_snapshots, s_view.snapshots_per_sweep);
    ui_set_text(s_footer, text);
}

static void reset_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sweep_reset_maxhold(s_band);
    ESP_LOGI(TAG, "max-hold reset (%s)", sweep_plan(s_band)->name);
}

static void paint(void)
{
    lv_refr_now(NULL); /* paint the static parts before the first direct plot draw */
    s_painted = true;
}

static void view_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    set_mode(!s_waterfall);
    paint(); /* labels and buttons changed outside the plot area */
    ESP_LOGI(TAG, "view -> %s", s_waterfall ? "waterfall" : "spectrum");
}

static void band_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_show_spectrum(s_band == BAND_24 ? BAND_5 : BAND_24);
}

/* BAND_COUNT keeps the current band. */
void ui_spectrum_enter(band_id_t band)
{
    if (band < BAND_COUNT && band != s_band) {
        s_band = band;
        build_axis();
        ESP_LOGI(TAG, "band -> %s", sweep_plan(s_band)->name);
    }
    s_wf_dirty = true;
    paint();
}

static lv_obj_t *add_button(const char *text, int32_t w, int32_t x_ofs, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *btn = lv_button_create(s_scr);
    lv_obj_set_size(btn, w, 22);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, x_ofs, -1);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (label) {
        *label = l;
    }
    return btn;
}

void ui_spectrum_leave(void)
{
    s_painted = false; /* another screen now owns the plot area */
}

lv_obj_t *ui_spectrum_create(void)
{
    unsigned max_bins = 0;
    for (int b = 0; b < BAND_COUNT; b++) {
        unsigned n = sweep_plan((band_id_t)b)->n_bins;
        max_bins = n > max_bins ? n : max_bins;
    }
    s_view.avg_cdb = heap_caps_malloc(max_bins * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_view.hold_cdb = heap_caps_malloc(max_bins * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_view.max_cdb = NULL;
    s_base = heap_caps_malloc(max_bins * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    for (int k = 0; k < 2; k++) {
        s_strip[k] = heap_caps_malloc(PLOT_W * STRIP_ROWS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_strip_free[k] = xSemaphoreCreateBinary();
        if (!s_strip[k] || !s_strip_free[k]) {
            ESP_LOGE(TAG, "plot strips");
            return NULL;
        }
        xSemaphoreGive(s_strip_free[k]);
    }
    for (int b = 0; b < BAND_COUNT; b++) {
        s_wf[b].px = heap_caps_malloc((size_t)PLOT_W * PLOT_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_wf[b].px) {
            ESP_LOGE(TAG, "waterfall");
            return NULL;
        }
    }
    if (!s_view.avg_cdb || !s_view.hold_cdb || !s_base) {
        ESP_LOGE(TAG, "buffers");
        return NULL;
    }
    build_lut();

    s_scr = ui_screen_create();
    s_header = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_header, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_header, 4, 2);
    lv_label_set_long_mode(s_header, LV_LABEL_LONG_MODE_DOTS);
    s_rates = lv_label_create(s_scr);
    lv_obj_align(s_rates, LV_ALIGN_TOP_RIGHT, -4, 5);

    for (int db = 80, i = 0; db >= 10; db -= 10, i++) {
        lv_obj_t *l = lv_label_create(s_scr);
        lv_label_set_text_fmt(l, "%d", db);
        lv_obj_set_pos(l, 2, PLOT_Y + PLOT_H - 1 - db * 100 * (PLOT_H - 1) / Y_MAX_CDB - 8);
        s_db_labels[i] = l;
    }
    for (int i = 0; i < WF_LABELS; i++) {
        lv_obj_t *l = lv_label_create(s_scr);
        lv_label_set_text(l, "");
        /* Clipped to the margin: a label redraw must never touch the plot. */
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_width(l, PLOT_X - 2);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(l, 1, PLOT_Y + i * (PLOT_H - 16) / (WF_LABELS - 1));
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        s_t_labels[i] = l;
    }

    s_axis = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_axis);
    lv_obj_set_pos(s_axis, 0, PLOT_Y + PLOT_H + 2);
    lv_obj_set_size(s_axis, BOARD_LCD_W, 34);

    s_footer = lv_label_create(s_scr);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    s_reset_btn = add_button("Reset max", 90, -2, reset_cb, NULL);
    add_button("2.4 / 5", 62, -96, band_cb, NULL);
    add_button("Waterfall", 86, -162, view_cb, &s_view_label);
    build_axis();
    return s_scr;
}
