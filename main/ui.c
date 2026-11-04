/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Screen switching, BOOT button, alarm border and beep, battery polling and
 * the refresh tick.
 */
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.h"
#include "board.h"
#include "detector.h"
#include "settings.h"
#include "lvgl_port.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui";

#define REFRESH_MS    100
#define BUTTON_MS     30
#define DEBOUNCE_POLL 2
#define LONG_POLL     50 /* 1.5 s at BUTTON_MS */
#define BORDER_PX     3
#define FLASH_TICKS   5  /* border on/off every 500 ms */
#define POWER_TICKS   50 /* battery read every 5 s */
#define BEEP_REPEAT_MS 15000 /* while any band stays in ALARM */

static lv_obj_t *s_scr[UI_SCREEN_COUNT];
static ui_screen_t s_cur = UI_STATUS;
/* Four thin bars rather than one full-screen frame: showing or hiding them
 * only invalidates the bars, never the directly drawn spectrum plot. */
static lv_obj_t *s_border[4];

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_color_t ui_state_colour(detect_state_t s)
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

void ui_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text); /* only invalidate when it changed */
    }
}

void ui_set_colour(lv_obj_t *label, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), c)) {
        lv_obj_set_style_text_color(label, c, 0);
    }
}

/* The result is interference, not proven jamming: say so. */
void ui_state_text(char *buf, size_t len, const detect_status_t *st)
{
    switch (st->state) {
    case DETECT_LEARNING:
        snprintf(buf, len, "learning baseline %u%%", st->learn_pct);
        break;
    case DETECT_NORMAL:
        snprintf(buf, len, "normal");
        break;
    case DETECT_SUSPECT:
        snprintf(buf, len, "suspect: %s", detect_kind_name(st->kind));
        break;
    case DETECT_ALARM:
        snprintf(buf, len, "INTERFERENCE: %s", detect_kind_name(st->kind));
        break;
    }
}

void ui_fmt_duration(char *buf, size_t len, uint32_t ms)
{
    const unsigned s = ms / 1000;
    if (s >= 3600) {
        snprintf(buf, len, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    } else {
        snprintf(buf, len, "%u:%02u", s / 60, s % 60);
    }
}

static void load(ui_screen_t screen)
{
    if (screen == s_cur && lv_screen_active() == s_scr[screen]) {
        return;
    }
    if (s_cur == UI_SETTINGS) {
        ui_settings_leave();
    } else if (s_cur == UI_SPECTRUM) {
        ui_spectrum_leave();
    }
    s_cur = screen;
    lv_screen_load(s_scr[screen]);
    if (screen == UI_SETTINGS) {
        ui_settings_enter();
    }
    ESP_LOGI(TAG, "screen %d", (int)screen);
}

void ui_show(ui_screen_t screen)
{
    if (screen == UI_SPECTRUM) {
        ui_show_spectrum(BAND_COUNT); /* keep the band */
        return;
    }
    load(screen);
}

void ui_show_spectrum(band_id_t band)
{
    load(UI_SPECTRUM);
    ui_spectrum_enter(band);
}

static void refresh_timer(lv_timer_t *t)
{
    LV_UNUSED(t);
    static unsigned tick;
    static board_power_t power;
    static bool power_ok, was_alarm;
    static uint32_t beep_ms;
    if (tick % POWER_TICKS == 0) {
        power_ok = board_power_read(&power) == ESP_OK;
    }
    ui_snap_t s = {.sweep = sweep_state(), .now_ms = (uint32_t)(esp_timer_get_time() / 1000), .power = power,
                   .power_ok = power_ok};
    bool alarm = false;
    for (int b = 0; b < BAND_COUNT; b++) {
        detector_get_status((band_id_t)b, &s.st[b]);
        s.scan[b] = sweep_scanned((band_id_t)b);
        alarm |= s.sweep == SWEEP_RUNNING && s.scan[b] && s.st[b].state == DETECT_ALARM;
    }
    ui_spectrum_feed(&s);
    if (alarm && (!was_alarm || s.now_ms - beep_ms >= BEEP_REPEAT_MS)) {
        settings_t set;
        settings_get(&set);
        if (set.beep) {
            audio_beep(3, 150, 100);
        }
        beep_ms = s.now_ms;
    }
    was_alarm = alarm;
    switch (s_cur) {
    case UI_STATUS:
        ui_status_update(&s);
        break;
    case UI_SPECTRUM:
        ui_spectrum_update(&s);
        break;
    case UI_SETTINGS:
        ui_settings_update(&s);
        break;
    default:
        break;
    }
    tick++;
    const bool on = alarm && (tick / FLASH_TICKS) % 2 == 0;
    for (int i = 0; i < 4; i++) {
        if (on == lv_obj_has_flag(s_border[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_flag(s_border[i], LV_OBJ_FLAG_HIDDEN, !on);
        }
    }
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
        ui_show((ui_screen_t)((s_cur + 1) % UI_SCREEN_COUNT));
    }
    held = 0;
    long_done = false;
}

esp_err_t ui_init(void)
{
    ESP_RETURN_ON_FALSE(lvp_lock(1000), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL),
                                           true, &lv_font_montserrat_14);
    lv_display_set_theme(disp, th);

    s_scr[UI_STATUS] = ui_status_create();
    s_scr[UI_SPECTRUM] = ui_spectrum_create();
    s_scr[UI_SETTINGS] = ui_settings_create();
    esp_err_t err = ESP_OK;
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        if (!s_scr[i]) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        static const int16_t r[4][4] = {
            {0, 0, BOARD_LCD_W, BORDER_PX},
            {0, BOARD_LCD_H - BORDER_PX, BOARD_LCD_W, BORDER_PX},
            {0, 0, BORDER_PX, BOARD_LCD_H},
            {BOARD_LCD_W - BORDER_PX, 0, BORDER_PX, BOARD_LCD_H},
        };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *o = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, r[i][0], r[i][1]);
            lv_obj_set_size(o, r[i][2], r[i][3]);
            lv_obj_set_style_bg_color(o, ui_state_colour(DETECT_ALARM), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
            s_border[i] = o;
        }
        load(UI_STATUS);
        lv_timer_create(refresh_timer, REFRESH_MS, NULL);
        lv_timer_create(button_timer, BUTTON_MS, NULL);
    }
    lvp_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "screens");
    return ESP_OK;
}
