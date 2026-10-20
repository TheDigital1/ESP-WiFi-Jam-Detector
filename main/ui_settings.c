/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings screen: re-learn, auto-range, defaults, detection thresholds,
 * sweep weighting, alarm beep and brightness. Changes apply when a slider
 * is released and are saved to flash SAVE_IDLE_MS after the last change, or
 * when the screen is left.
 *
 * Three pages behind tabs rather than one scrolling list: a full redraw of
 * the list costs ~80 ms of CPU on this core, so touch scrolling ran at a few
 * frames/s and starved the sweep. A page change is one redraw.
 */
#include <stdio.h>
#include "esp_log.h"
#include "audio.h"
#include "board.h"
#include "eventlog.h"
#include "detector.h"
#include "settings.h"
#include "ui_priv.h"

static const char *TAG = "ui_settings";

#define SAVE_IDLE_MS 2000
#define ROW_H        42
#define LABEL_W      210
#define TOP_H        40

typedef enum {
    P_GENERAL,
    P_DETECTION,
    P_TIMING,
    P_COUNT
} page_t;

static const char *const s_page_name[P_COUNT] = {"General", "Detection", "Timing"};

typedef enum {
    F_LEARN,
    F_WIDE_DB,
    F_WIDE_PCT,
    F_CW_DB,
    F_CW_PCT,
    F_SWEPT_DB,
    F_SWEPT_PCT,
    F_DUTY,
    F_HOLD,
    F_CLEAR,
    F_WEIGHT,
    F_BRIGHT,
    F_COUNT
} field_t;

typedef struct {
    const char *name, *unit;
    int min, max, step;
    page_t page;
} field_info_t;

/* Slider ranges are tighter than detect_check_config(): only sensible values. */
static const field_info_t s_info[F_COUNT] = {
    [F_LEARN] = {"Learn time", "s", 10, 300, 10, P_TIMING},
    [F_WIDE_DB] = {"Wideband rise", "dB", 3, 30, 1, P_DETECTION},
    [F_WIDE_PCT] = {"Wideband share", "%", 20, 100, 5, P_DETECTION},
    [F_CW_DB] = {"Narrowband rise", "dB", 6, 40, 1, P_DETECTION},
    [F_CW_PCT] = {"Narrowband persist", "%", 50, 100, 5, P_DETECTION},
    [F_SWEPT_DB] = {"Swept rise", "dB", 6, 40, 1, P_DETECTION},
    [F_SWEPT_PCT] = {"Swept coverage", "%", 20, 100, 5, P_DETECTION},
    [F_DUTY] = {"Alarm duty", "%", 30, 100, 5, P_TIMING},
    [F_HOLD] = {"Alarm after", "s", 2, 20, 1, P_TIMING},
    [F_CLEAR] = {"Clear after", "s", 3, 60, 1, P_TIMING},
    [F_WEIGHT] = {"2.4 sweeps per 5 GHz", "", 1, 6, 1, P_GENERAL},
    [F_BRIGHT] = {"Brightness", "%", 10, 100, 5, P_GENERAL},
};

static settings_t s_work;
static lv_obj_t *s_slider[F_COUNT], *s_value[F_COUNT];
static lv_obj_t *s_beep, *s_info_label;
static lv_obj_t *s_page[P_COUNT], *s_tab[P_COUNT];
static page_t s_cur_page;
static bool s_dirty;
static uint32_t s_changed_ms;

static int get(const settings_t *s, field_t f)
{
    const detect_config_t *c = &s->det;
    switch (f) {
    case F_LEARN: return c->learn_s;
    case F_WIDE_DB: return c->wide_cdb / 100;
    case F_WIDE_PCT: return c->wide_pct;
    case F_CW_DB: return c->cw_cdb / 100;
    case F_CW_PCT: return c->cw_pct;
    case F_SWEPT_DB: return c->swept_cdb / 100;
    case F_SWEPT_PCT: return c->swept_cover_pct;
    case F_DUTY: return c->duty_pct;
    case F_HOLD: return c->hold_s;
    case F_CLEAR: return c->clear_s;
    case F_WEIGHT: return s->weight[BAND_24];
    case F_BRIGHT: return s->brightness;
    default: return 0;
    }
}

static void set(settings_t *s, field_t f, int v)
{
    detect_config_t *c = &s->det;
    switch (f) {
    case F_LEARN: c->learn_s = (uint16_t)v; break;
    case F_WIDE_DB: c->wide_cdb = (int16_t)(v * 100); break;
    case F_WIDE_PCT: c->wide_pct = (uint8_t)v; break;
    case F_CW_DB: c->cw_cdb = (int16_t)(v * 100); break;
    case F_CW_PCT: c->cw_pct = (uint8_t)v; break;
    case F_SWEPT_DB: c->swept_cdb = (int16_t)(v * 100); break;
    case F_SWEPT_PCT: c->swept_cover_pct = (uint8_t)v; break;
    case F_DUTY: c->duty_pct = (uint8_t)v; break;
    case F_HOLD: c->hold_s = (uint16_t)v; break;
    case F_CLEAR: c->clear_s = (uint16_t)v; break;
    case F_WEIGHT: s->weight[BAND_24] = (uint8_t)v; s->weight[BAND_5] = 1; break;
    case F_BRIGHT: s->brightness = (uint8_t)v; break;
    default: break;
    }
}

static int clamp_step(field_t f, int v)
{
    const field_info_t *i = &s_info[f];
    v = (v - i->min + i->step / 2) / i->step * i->step + i->min;
    return v < i->min ? i->min : (v > i->max ? i->max : v);
}

static void show_value(field_t f, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %s", v, s_info[f].unit);
    ui_set_text(s_value[f], buf);
}

/* Push the working copy to the detector, sweep and backlight. */
static void apply(void)
{
    if (settings_check(&s_work) != 0) {
        ESP_LOGW(TAG, "settings out of range, not applied");
        return;
    }
    const unsigned w[BAND_COUNT] = {s_work.weight[BAND_24], s_work.weight[BAND_5]};
    esp_err_t err = detector_set_config(&s_work.det);
    if (err == ESP_OK) {
        err = sweep_set_weights(w);
    }
    if (err == ESP_OK) {
        err = board_backlight_set(s_work.brightness);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply: %s", esp_err_to_name(err));
    }
}

static void changed(void)
{
    s_dirty = true;
    s_changed_ms = lv_tick_get();
}

static void save(void)
{
    if (!s_dirty) {
        return;
    }
    settings_t cur;
    settings_get(&cur); /* gains may have changed (auto-range) since the copy was taken */
    for (int b = 0; b < BAND_COUNT; b++) {
        s_work.gain[b] = cur.gain[b];
    }
    const esp_err_t err = settings_set(&s_work);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save: %s", esp_err_to_name(err));
    }
    s_dirty = false;
}

static void slider_cb(lv_event_t *e)
{
    const field_t f = (field_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sl = lv_event_get_target(e);
    const int v = clamp_step(f, lv_slider_get_value(sl));
    show_value(f, v);
    if (f == F_BRIGHT) {
        board_backlight_set((uint8_t)v); /* live: an I2C write */
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && get(&s_work, f) != v) {
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        set(&s_work, f, v);
        apply();
        changed();
        ESP_LOGI(TAG, "%s = %d %s", s_info[f].name, v, s_info[f].unit);
    }
}

static void beep_cb(lv_event_t *e)
{
    s_work.beep = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    changed();
}

static void test_beep_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    audio_beep(3, 150, 100);
}

static void load_controls(void)
{
    for (int f = 0; f < F_COUNT; f++) {
        const int v = get(&s_work, (field_t)f);
        lv_slider_set_value(s_slider[f], v, LV_ANIM_OFF);
        show_value((field_t)f, v);
    }
    lv_obj_set_state(s_beep, LV_STATE_CHECKED, s_work.beep);
}

static void relearn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ESP_LOGI(TAG, "re-learn baselines");
    detector_relearn();
    ui_show(UI_STATUS);
}

static void autorange_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ESP_LOGI(TAG, "auto-range requested");
    sweep_autorange(); /* the new gains re-learn the baselines (app_main.c) */
    ui_show(UI_STATUS);
}

static void defaults_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    settings_t d;
    settings_default(&d);
    for (int b = 0; b < BAND_COUNT; b++) {
        d.gain[b] = s_work.gain[b];
    }
    s_work = d;
    load_controls();
    apply();
    changed();
    ESP_LOGI(TAG, "defaults restored");
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 34);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static void show_page(page_t p)
{
    for (int i = 0; i < P_COUNT; i++) {
        lv_obj_set_flag(s_page[i], LV_OBJ_FLAG_HIDDEN, i != (int)p);
        lv_obj_set_state(s_tab[i], LV_STATE_CHECKED, i == (int)p);
    }
    s_cur_page = p;
}

static void tab_cb(lv_event_t *e)
{
    const page_t p = (page_t)(uintptr_t)lv_event_get_user_data(e);
    if (p != s_cur_page) {
        show_page(p); /* tabs are not CHECKABLE: show_page() owns their state */
    }
}

static lv_obj_t *add_page(lv_obj_t *scr)
{
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, TOP_H);
    lv_obj_set_size(page, BOARD_LCD_W, BOARD_LCD_H - TOP_H);
    lv_obj_set_style_pad_hor(page, 12, 0);
    lv_obj_set_style_pad_top(page, 4, 0);
    lv_obj_set_style_pad_row(page, 2, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static lv_obj_t *add_row(lv_obj_t *list, const char *name)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, -8);
    return row;
}

lv_obj_t *ui_settings_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_set_pos(title, 8, 9);
    for (int p = 0; p < P_COUNT; p++) {
        lv_obj_t *tab = lv_button_create(scr);
        lv_obj_set_size(tab, 106, 32);
        lv_obj_set_pos(tab, BOARD_LCD_W - 4 - (P_COUNT - p) * 112, 4);
        /* unchecked tabs are dim; the checked one keeps the theme colour */
        lv_obj_set_style_bg_color(tab, lv_color_hex(0x2a3038), 0);
        lv_obj_set_style_bg_color(tab, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
        lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)p);
        lv_obj_t *l = lv_label_create(tab);
        lv_label_set_text(l, s_page_name[p]);
        lv_obj_center(l);
        s_tab[p] = tab;
        s_page[p] = add_page(scr);
    }

    lv_obj_t *list = s_page[P_GENERAL];
    lv_obj_t *buttons = lv_obj_create(list);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, lv_pct(100), 40);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    add_button(buttons, "Re-learn baseline", relearn_cb);
    add_button(buttons, "Auto-range gain", autorange_cb);
    add_button(buttons, "Defaults", defaults_cb);

    s_info_label = lv_label_create(list);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0x9ab0c8), 0);
    lv_obj_set_width(s_info_label, lv_pct(100));
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_info_label, "");

    for (int f = 0; f < F_COUNT; f++) {
        lv_obj_t *row = add_row(s_page[s_info[f].page], s_info[f].name);
        s_value[f] = lv_label_create(row);
        lv_obj_align(s_value[f], LV_ALIGN_LEFT_MID, 0, 9);
        lv_obj_set_style_text_color(s_value[f], lv_color_hex(0x35d0ff), 0);
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_width(sl, BOARD_LCD_W - 24 - LABEL_W - 16);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_slider_set_range(sl, s_info[f].min, s_info[f].max);
        /* VALUE_CHANGED updates the label, RELEASED applies */
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)f);
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)f);
        s_slider[f] = sl;
    }
    lv_obj_t *row = add_row(s_page[P_GENERAL], "Alarm beep");
    s_beep = lv_switch_create(row);
    lv_obj_align(s_beep, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t *test = add_button(row, "Test", test_beep_cb);
    lv_obj_set_size(test, 70, 32);
    lv_obj_align(test, LV_ALIGN_RIGHT_MID, -80, 0);
    lv_obj_add_event_cb(s_beep, beep_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = lv_label_create(s_page[P_GENERAL]);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9ab0c8), 0);
    lv_label_set_text(hint, "BOOT: next screen   hold BOOT: re-learn");

    show_page(P_GENERAL);
    settings_get(&s_work);
    load_controls();
    return scr;
}

void ui_settings_enter(void)
{
    settings_get(&s_work);
    load_controls();
    s_dirty = false;
}

void ui_settings_leave(void)
{
    save();
}

void ui_settings_update(const ui_snap_t *s)
{
    char text[200], sd[64], pwr[48];
    sweep_view_t v[BAND_COUNT] = {0};
    for (int b = 0; b < BAND_COUNT; b++) {
        sweep_get_view((band_id_t)b, &v[b]);
    }
    if (s->sweep == SWEEP_AUTORANGE) {
        snprintf(text, sizeof(text), "auto-ranging gain...");
    } else {
        eventlog_status_t ev;
        eventlog_get_status(&ev);
        if (ev.mounted) {
            snprintf(sd, sizeof(sd), "%u events logged%s, %u MB free", (unsigned)ev.written,
                     ev.errors ? " (errors)" : "", (unsigned)(ev.free_kb / 1024));
        } else {
            snprintf(sd, sizeof(sd), "no card");
        }
        if (!s->power_ok) {
            snprintf(pwr, sizeof(pwr), "unknown");
        } else if (s->power.battery) {
            snprintf(pwr, sizeof(pwr), "battery %d%% %.2f V%s", s->power.percent, s->power.battery_mv / 1000.0,
                     s->power.charging ? ", charging" : "");
        } else {
            snprintf(pwr, sizeof(pwr), "USB, no battery");
        }
        snprintf(text, sizeof(text), "Gain 2.4 GHz %u, 5 GHz %u (stored). Baselines %s / %s.\nTF card: %s. Power: %s.",
                 v[BAND_24].gain, v[BAND_5].gain, s->st[BAND_24].state == DETECT_LEARNING ? "learning" : "learned",
                 s->st[BAND_5].state == DETECT_LEARNING ? "learning" : "learned", sd, pwr);
    }
    ui_set_text(s_info_label, text);
    if (s_dirty && lv_tick_elaps(s_changed_ms) >= SAVE_IDLE_MS) {
        save();
    }
}
