/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M4 spectrum screen, 480x320 landscape:
 *   header  : band and its detector state (coloured); the other band's state
 *   plot    : drawn by plot.c straight to the panel (not through LVGL):
 *             current average (cyan, filled), max-hold (orange) and learned
 *             baseline (grey), relative dB
 *   axis    : band edges in MHz, Wi-Fi channel ticks, U-NII labels
 *   footer  : gain / timing / rate / clipping, "Reset max" button
 * BOOT short press switches band; long press (1.5 s) re-learns the baselines.
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
#include "lvgl_port.h"
#include "plot.h"
#include "sweep.h"
#include "ui.h"

static const char *TAG = "ui";

#define PLOT_X        28
#define PLOT_Y        26
#define PLOT_W        (BOARD_LCD_W - PLOT_X - 2)
#define PLOT_H        236
#define Y_MIN_CDB     0
#define Y_MAX_CDB     9000
#define REFRESH_MS    100
#define BUTTON_MS     30
#define DEBOUNCE_POLL 2
#define LONG_POLL     50 /* 1.5 s at BUTTON_MS */
#define STRIP_ROWS    12 /* plot strip: 450 x 12 px = 10.8 KB; two internal DMA buffers */

/* The plot area is not an LVGL object: LVGL only paints it once (screen
 * background) and never invalidates it again. A full LVGL redraw of this
 * area cost ~60 ms of CPU per frame on this single core; drawing it directly
 * costs a fraction of that. */
static lv_obj_t *s_header, *s_rates, *s_footer, *s_axis;
static uint16_t *s_strip[2];
static SemaphoreHandle_t s_strip_free[2];
/* Column data lives in PSRAM: internal SRAM is reserved for the capture
 * bank and Wi-Fi (see components/espsdr_rx). */
EXT_RAM_BSS_ATTR static int16_t s_col_avg[PLOT_W], s_col_hold[PLOT_W], s_col_base[PLOT_W];
EXT_RAM_BSS_ATTR static plot_col_t s_cols[PLOT_W];
static uint16_t s_grid_x[16];
static unsigned s_n_grid_x;
static band_id_t s_band = BAND_24;
static sweep_view_t s_view;
static int16_t *s_base; /* detector baseline, n_bins */
static lv_obj_t *s_axis_labels[16];
static unsigned s_n_axis_labels;

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

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text); /* only invalidate when it changed */
    }
}

static void strip_done(void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Render strip n+1 while strip n is still being sent. */
static esp_err_t draw_plot(void)
{
    static const plot_buf_t geom = {.w = PLOT_W, .h = PLOT_H};
    plot_frame_t f = {.geom = &geom, .st = &s_style, .cols = s_cols};
    plot_prepare(&f, s_col_avg, s_col_hold, s_col_base, s_grid_x, s_n_grid_x);
    esp_err_t err = ESP_OK;
    unsigned i = 0;
    for (unsigned y0 = 0; y0 < PLOT_H && err == ESP_OK; y0 += STRIP_ROWS, i ^= 1) {
        unsigned rows = y0 + STRIP_ROWS <= PLOT_H ? STRIP_ROWS : PLOT_H - y0;
        if (xSemaphoreTake(s_strip_free[i], pdMS_TO_TICKS(500)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        plot_rows(&f, y0, rows, s_strip[i], PLOT_W);
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

static int32_t khz_to_col(const band_plan_t *p, uint32_t khz)
{
    return (int32_t)((int64_t)(khz - p->start_khz) * (PLOT_W - 1) / (p->stop_khz - p->start_khz));
}

static void add_axis_label(const char *text, int32_t x, int32_t y)
{
    if (s_n_axis_labels >= sizeof(s_axis_labels) / sizeof(s_axis_labels[0])) {
        return;
    }
    lv_obj_t *l = lv_label_create(s_axis);
    lv_label_set_text(l, text);
    lv_obj_update_layout(l);
    int32_t w = lv_obj_get_width(l);
    int32_t left = x - w / 2;
    left = left < 0 ? 0 : (left + w > BOARD_LCD_W ? BOARD_LCD_W - w : left);
    lv_obj_set_pos(l, left, y);
    s_axis_labels[s_n_axis_labels++] = l;
}

static void build_axis(void)
{
    const band_plan_t *p = sweep_plan(s_band);
    for (unsigned i = 0; i < s_n_axis_labels; i++) {
        lv_obj_delete(s_axis_labels[i]);
    }
    s_n_axis_labels = 0;
    s_n_grid_x = 0;
    char buf[24];
    snprintf(buf, sizeof(buf), "%u", (unsigned)(p->start_khz / 1000));
    add_axis_label(buf, PLOT_X, 0);
    snprintf(buf, sizeof(buf), "%u", (unsigned)((p->stop_khz + 500) / 1000));
    add_axis_label(buf, PLOT_X + PLOT_W, 0);
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
                       16);
    }
    if (!p->n_labels) {
        add_axis_label("MHz", BOARD_LCD_W / 2, 16);
    }
}

static lv_color_t state_colour(detect_state_t s)
{
    switch (s) {
    case DETECT_NORMAL:
        return lv_color_hex(0x3ddc84);
    case DETECT_SUSPECT:
        return lv_color_hex(0xffb020);
    case DETECT_ALARM:
        return lv_color_hex(0xff4040);
    default:
        return lv_color_hex(0x9ab0c8);
    }
}

static void set_colour(lv_obj_t *label, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), c)) {
        lv_obj_set_style_text_color(label, c, 0);
    }
}

/* The result is interference, not proven jamming: say so. */
static void state_text(char *buf, size_t len, const char *band, const detect_status_t *st)
{
    switch (st->state) {
    case DETECT_LEARNING:
        snprintf(buf, len, "%s  learning baseline %u%%", band, st->learn_pct);
        break;
    case DETECT_NORMAL:
        snprintf(buf, len, "%s  normal", band);
        break;
    case DETECT_SUSPECT:
        snprintf(buf, len, "%s  suspect: %s", band, detect_kind_name(st->kind));
        break;
    case DETECT_ALARM:
        snprintf(buf, len, "%s  INTERFERENCE: %s", band, detect_kind_name(st->kind));
        break;
    }
}

static const char *short_state(const detect_status_t *st)
{
    static const char *names[] = {"learning", "normal", "suspect", "INTERFERENCE"};
    return names[st->state];
}

static void update_view(void)
{
    const band_plan_t *p = sweep_plan(s_band);
    if (sweep_get_view(s_band, &s_view) != ESP_OK) {
        return;
    }
    detect_status_t st[BAND_COUNT];
    for (int b = 0; b < BAND_COUNT; b++) {
        detector_get_status((band_id_t)b, &st[b]);
    }
    const bool have_base = st[s_band].state != DETECT_LEARNING && detector_get_baseline(s_band, s_base) == ESP_OK;
    for (unsigned x = 0; x < PLOT_W; x++) {
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
    esp_err_t err = draw_plot();
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "plot draw: %s", esp_err_to_name(err));
    }
    draw_us += t1 - t0;
    frames++;
    if (t1 - window >= 10000000) {
        ESP_LOGI(TAG, "spectrum %.1f frames/s, %.1f ms per frame (render + LCD)", frames * 1e6f / (t1 - window),
                 draw_us / 1000.0f / frames);
        draw_us = 0;
        frames = 0;
        window = t1;
    }

    char text[96];
    const sweep_state_t sw = sweep_state();
    if (sw == SWEEP_RUNNING) {
        state_text(text, sizeof(text), p->name, &st[s_band]);
    } else {
        snprintf(text, sizeof(text), "%s  %s", p->name, sw == SWEEP_FAILED ? "SWEEP FAILED" : "auto-ranging gain...");
    }
    set_text(s_header, text);
    set_colour(s_header, sw == SWEEP_RUNNING ? state_colour(st[s_band].state) : lv_color_white());
    const band_id_t other = s_band == BAND_24 ? BAND_5 : BAND_24;
    snprintf(text, sizeof(text), "%s: %s", sweep_plan(other)->name,
             sw == SWEEP_RUNNING ? short_state(&st[other]) : "-");
    set_text(s_rates, text);
    set_colour(s_rates, sw == SWEEP_RUNNING ? state_colour(st[other].state) : lv_color_white());
    /* Sweep time rounded to 10 ms so the footer does not redraw every frame. */
    snprintf(text, sizeof(text), "gain %u   sweep %u ms   %.1f/s   clipped %u/%u", s_view.gain,
             (unsigned)(s_view.last_sweep_ms + 5) / 10 * 10, s_view.sweeps_per_10s / 10.0f,
             s_view.clipped_snapshots, s_view.snapshots_per_sweep);
    set_text(s_footer, text);
}

static void refresh_timer(lv_timer_t *t)
{
    LV_UNUSED(t);
    update_view();
}

static void button_timer(lv_timer_t *t)
{
    LV_UNUSED(t);
    static unsigned held;
    static bool long_done;
    if (board_boot_pressed()) {
        if (++held >= LONG_POLL && !long_done) {
            long_done = true;
            ESP_LOGI(TAG, "BOOT long press: re-learn baselines");
            detector_relearn();
        }
        return;
    }
    if (held >= DEBOUNCE_POLL && !long_done) {
        s_band = s_band == BAND_24 ? BAND_5 : BAND_24;
        ESP_LOGI(TAG, "band -> %s", sweep_plan(s_band)->name);
        build_axis();
        update_view();
    }
    held = 0;
    long_done = false;
}

static void reset_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sweep_reset_maxhold(s_band);
    ESP_LOGI(TAG, "max-hold reset (%s)", sweep_plan(s_band)->name);
}

esp_err_t ui_init(void)
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
        ESP_RETURN_ON_FALSE(s_strip[k] && s_strip_free[k], ESP_ERR_NO_MEM, TAG, "plot strips");
        xSemaphoreGive(s_strip_free[k]);
    }
    ESP_RETURN_ON_FALSE(s_view.avg_cdb && s_view.hold_cdb && s_base, ESP_ERR_NO_MEM, TAG, "ui buffers");

    ESP_RETURN_ON_FALSE(lvp_lock(1000), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = lv_label_create(scr);
    lv_obj_set_style_text_font(s_header, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_header, 4, 2);
    s_rates = lv_label_create(scr);
    lv_obj_align(s_rates, LV_ALIGN_TOP_RIGHT, -4, 5);

    for (int db = 80; db >= 10; db -= 10) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text_fmt(l, "%d", db);
        lv_obj_set_pos(l, 2, PLOT_Y + PLOT_H - 1 - db * 100 * (PLOT_H - 1) / Y_MAX_CDB - 8);
    }

    s_axis = lv_obj_create(scr);
    lv_obj_remove_style_all(s_axis);
    lv_obj_set_pos(s_axis, 0, PLOT_Y + PLOT_H + 2);
    lv_obj_set_size(s_axis, BOARD_LCD_W, 34);

    s_footer = lv_label_create(scr);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 100, 22);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -2, -1);
    lv_obj_add_event_cb(btn, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Reset max");
    lv_obj_center(bl);

    build_axis();
    lv_refr_now(NULL); /* paint the static parts before the first direct plot draw */
    update_view();
    lv_timer_create(refresh_timer, REFRESH_MS, NULL);
    lv_timer_create(button_timer, BUTTON_MS, NULL);
    lvp_unlock();
    return ESP_OK;
}
