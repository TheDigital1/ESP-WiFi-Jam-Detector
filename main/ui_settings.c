/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings screen: re-learn, auto-range, defaults, detection thresholds,
 * bands scanned, sweep weighting, alarm beep and brightness. Changes apply when a slider
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
#include "lvgl_port.h"
#include "params.h"
#include "settings.h"
#include "ui.h"
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

static const char *const s_page_name[P_COUNT] = {"General", "Detection", "Scan & timing"};

/* Page of each slider (params.c has names, keys and ranges). */
static const page_t s_param_page[PARAM_COUNT] = {
    [PARAM_LEARN] = P_TIMING,     [PARAM_WIDE_DB] = P_DETECTION,  [PARAM_WIDE_PCT] = P_DETECTION,
    [PARAM_CW_DB] = P_DETECTION,  [PARAM_CW_PCT] = P_DETECTION,   [PARAM_SWEPT_DB] = P_DETECTION,
    [PARAM_SWEPT_PCT] = P_DETECTION, [PARAM_DUTY] = P_TIMING,     [PARAM_HOLD] = P_TIMING,
    [PARAM_CLEAR] = P_TIMING,     [PARAM_WEIGHT] = P_TIMING,      [PARAM_BRIGHT] = P_GENERAL,
};

static settings_t s_work;
static lv_obj_t *s_slider[PARAM_COUNT], *s_value[PARAM_COUNT];
static lv_obj_t *s_beep, *s_info_label, *s_scan[BAND_COUNT];
static lv_obj_t *s_page[P_COUNT], *s_tab[P_COUNT];
static page_t s_cur_page;
static bool s_dirty;
static uint32_t s_changed_ms;

static void show_value(param_t f, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %s", v, param_info[f].unit);
    ui_set_text(s_value[f], buf);
}

/* Push settings to the detector, sweep and backlight (not saved). */
static esp_err_t apply(const settings_t *set)
{
    if (settings_check(set) != 0) {
        ESP_LOGW(TAG, "settings out of range, not applied");
        return ESP_ERR_INVALID_ARG;
    }
    const unsigned w[BAND_COUNT] = {set->weight[BAND_24], set->weight[BAND_5]};
    esp_err_t err = detector_set_config(&set->det);
    if (err == ESP_OK) {
        err = sweep_set_weights(w);
    }
    if (err == ESP_OK) {
        detector_set_scan(set->scan); /* before the sweep: no stale alarm in between */
        err = sweep_set_scan(set->scan);
    }
    if (err == ESP_OK) {
        err = board_backlight_set(set->brightness);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply: %s", esp_err_to_name(err));
    }
    return err;
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
    const param_t f = (param_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sl = lv_event_get_target(e);
    const int v = param_clamp(f, lv_slider_get_value(sl));
    show_value(f, v);
    if (f == PARAM_BRIGHT) {
        board_backlight_set((uint8_t)v); /* live: an I2C write */
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && param_get(&s_work, f) != v) {
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        param_set(&s_work, f, v);
        apply(&s_work);
        changed();
        ESP_LOGI(TAG, "%s = %d %s", param_info[f].name, v, param_info[f].unit);
    }
}

static void beep_cb(lv_event_t *e)
{
    s_work.beep = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    changed();
}

static void scan_cb(lv_event_t *e)
{
    const band_id_t b = (band_id_t)(uintptr_t)lv_event_get_user_data(e);
    const band_id_t other = b == BAND_24 ? BAND_5 : BAND_24;
    const bool on = lv_obj_has_state(s_scan[b], LV_STATE_CHECKED);
    if (!on && !s_work.scan[other]) {
        lv_obj_add_state(s_scan[b], LV_STATE_CHECKED); /* one band always stays on */
        return;
    }
    s_work.scan[b] = on;
    apply(&s_work);
    changed();
    ESP_LOGI(TAG, "scan %s %s", sweep_plan(b)->name, on ? "on" : "off");
}

static void test_beep_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    audio_beep(3, 150, 100);
}

static void load_controls(void)
{
    for (int f = 0; f < PARAM_COUNT; f++) {
        const int v = param_get(&s_work, (param_t)f);
        lv_slider_set_value(s_slider[f], v, LV_ANIM_OFF);
        show_value((param_t)f, v);
    }
    lv_obj_set_state(s_beep, LV_STATE_CHECKED, s_work.beep);
    for (int b = 0; b < BAND_COUNT; b++) {
        lv_obj_set_state(s_scan[b], LV_STATE_CHECKED, s_work.scan[b]);
    }
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
    apply(&s_work);
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
    static const int32_t tab_w[P_COUNT] = {96, 100, 130}; /* sum + gaps clear the title */
    for (int p = 0, x = BOARD_LCD_W - 4 - (96 + 100 + 130 + 2 * 6); p < P_COUNT; x += tab_w[p] + 6, p++) {
        lv_obj_t *tab = lv_button_create(scr);
        lv_obj_set_size(tab, tab_w[p], 32);
        lv_obj_set_pos(tab, x, 4);
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

    /* First on its page: which bands are swept. Checkable buttons, lit when on. */
    lv_obj_t *scan_row = add_row(s_page[P_TIMING], "Scan bands");
    for (int b = BAND_COUNT - 1, x = -8; b >= 0; b--, x -= 96) {
        lv_obj_t *btn = lv_button_create(scan_row);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, sweep_plan((band_id_t)b)->name);
        lv_obj_center(l);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_size(btn, 88, 32);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, x, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3038), 0);
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, scan_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)b);
        s_scan[b] = btn;
    }

    for (int f = 0; f < PARAM_COUNT; f++) {
        lv_obj_t *row = add_row(s_page[s_param_page[f]], param_info[f].name);
        s_value[f] = lv_label_create(row);
        lv_obj_align(s_value[f], LV_ALIGN_LEFT_MID, 0, 9);
        lv_obj_set_style_text_color(s_value[f], lv_color_hex(0x35d0ff), 0);
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_width(sl, BOARD_LCD_W - 24 - LABEL_W - 16);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_slider_set_range(sl, param_info[f].min, param_info[f].max);
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
            snprintf(sd, sizeof(sd), "%u events, %u images%s, %u MB free", (unsigned)ev.written,
                     (unsigned)ev.images, ev.errors ? " (errors)" : "", (unsigned)(ev.free_kb / 1024));
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

esp_err_t ui_settings_set_key(const char *key, int value)
{
    if (!lvp_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    save(); /* edits made on the screen first, so they are not lost or reverted */
    settings_t set;
    settings_get(&set);
    esp_err_t err = param_set_key(&set, key, value) == 0 && settings_check(&set) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        err = apply(&set);
    }
    if (err == ESP_OK) {
        err = settings_set(&set);
    }
    if (err == ESP_OK) {
        s_work = set; /* the screen shows the new value, whether visible now or later */
        load_controls();
        s_dirty = false;
        ESP_LOGI(TAG, "remote: %s = %d", key, value);
    }
    lvp_unlock();
    return err;
}
