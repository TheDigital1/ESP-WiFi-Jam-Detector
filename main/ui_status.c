/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Status screen (default): one tile per band with its state colour, floor
 * rise against the baseline, an evidence duty bar and the last alarm (as
 * uptime: the RTC is never set, since Wi-Fi is off while scanning). Tap a
 * tile for its spectrum.
 */
#include <stdio.h>
#include "board.h"
#include "ui_priv.h"

#define TILE_Y 32
#define TILE_W 232
#define TILE_H 244
#define PAD    10

typedef struct {
    lv_obj_t *tile, *state, *why, *floor, *duty_bar, *duty, *last, *rate;
    detect_state_t shown; /* tile colours currently applied */
    bool coloured;
} tile_t;

static lv_obj_t *s_uptime;
static tile_t s_tile[BAND_COUNT];

static void tile_cb(lv_event_t *e)
{
    ui_show_spectrum((band_id_t)(uintptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_width(l, TILE_W - 2 * PAD);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void build_tile(lv_obj_t *scr, band_id_t band)
{
    tile_t *t = &s_tile[band];
    t->tile = lv_obj_create(scr);
    lv_obj_set_pos(t->tile, band == BAND_24 ? 6 : BOARD_LCD_W - 6 - TILE_W, TILE_Y);
    lv_obj_set_size(t->tile, TILE_W, TILE_H);
    lv_obj_set_style_pad_all(t->tile, PAD, 0);
    lv_obj_set_style_radius(t->tile, 8, 0);
    lv_obj_set_style_border_width(t->tile, 2, 0);
    lv_obj_remove_flag(t->tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t->tile, tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)band);

    lv_obj_t *name = add_label(t->tile, &lv_font_montserrat_28, 0);
    lv_label_set_text(name, sweep_plan(band)->name);
    t->state = add_label(t->tile, &lv_font_montserrat_20, 34);
    t->why = add_label(t->tile, &lv_font_montserrat_14, 60);
    lv_label_set_long_mode(t->why, LV_LABEL_LONG_MODE_WRAP);
    t->floor = add_label(t->tile, &lv_font_montserrat_14, 98);
    t->duty = add_label(t->tile, &lv_font_montserrat_14, 120);
    t->duty_bar = lv_bar_create(t->tile);
    lv_obj_set_pos(t->duty_bar, 0, 140);
    lv_obj_set_size(t->duty_bar, TILE_W - 2 * PAD, 12);
    lv_bar_set_range(t->duty_bar, 0, 100);
    t->last = add_label(t->tile, &lv_font_montserrat_14, 160);
    t->rate = add_label(t->tile, &lv_font_montserrat_14, 198);
    lv_obj_set_style_text_color(t->rate, lv_color_hex(0x9ab0c8), 0);
}

lv_obj_t *ui_status_create(void)
{
    lv_obj_t *scr = ui_screen_create();
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "RF interference monitor");
    lv_obj_set_pos(title, 8, 4);
    s_uptime = lv_label_create(scr);
    lv_obj_align(s_uptime, LV_ALIGN_TOP_RIGHT, -8, 8);
    for (int b = 0; b < BAND_COUNT; b++) {
        build_tile(scr, (band_id_t)b);
    }
    lv_obj_t *note = lv_label_create(scr);
    lv_obj_set_style_text_color(note, lv_color_hex(0x9ab0c8), 0);
    lv_label_set_text(note, "Interference / possible jamming: this cannot prove intent.\n"
                            "BOOT: next screen   hold BOOT: re-learn   tap a band: spectrum");
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    return scr;
}

static void colour_tile(tile_t *t, detect_state_t s)
{
    if (t->coloured && t->shown == s) {
        return;
    }
    const lv_color_t c = ui_state_colour(s);
    lv_obj_set_style_bg_color(t->tile, lv_color_mix(c, lv_color_black(), s >= DETECT_SUSPECT ? 70 : 35), 0);
    lv_obj_set_style_border_color(t->tile, c, 0);
    lv_obj_set_style_text_color(t->state, c, 0);
    lv_obj_set_style_bg_color(t->duty_bar, c, LV_PART_INDICATOR);
    t->shown = s;
    t->coloured = true;
}

static void why_text(char *buf, size_t len, const detect_status_t *st, uint32_t now_ms)
{
    char d[16];
    switch (st->state) {
    case DETECT_LEARNING:
        snprintf(buf, len, "keep conditions normal while the baseline is learned");
        break;
    case DETECT_NORMAL:
        snprintf(buf, len, "nothing unusual against the baseline");
        break;
    case DETECT_SUSPECT:
        snprintf(buf, len, st->kind == DETECT_PULSED ? "pulsed energy (busy Wi-Fi or microwave oven?)"
                                                     : "%s evidence, watching", detect_kind_name(st->kind));
        break;
    case DETECT_ALARM:
        ui_fmt_duration(d, sizeof(d), now_ms - st->state_since_ms);
        snprintf(buf, len, "possible jamming: %s, for %s", detect_kind_name(st->kind), d);
        break;
    }
}

void ui_status_update(const ui_snap_t *s)
{
    char text[96], d[16];
    ui_fmt_duration(d, sizeof(d), s->now_ms);
    char pwr[32] = "";
    if (s->power_ok && s->power.battery && s->power.percent >= 0) {
        snprintf(pwr, sizeof(pwr), "%s %d%%   ", s->power.charging ? "charging" : "battery", s->power.percent);
    } else if (s->power_ok && s->power.vbus) {
        snprintf(pwr, sizeof(pwr), "USB   ");
    }
    snprintf(text, sizeof(text), "%sup %s", pwr, d);
    ui_set_text(s_uptime, text);

    for (int b = 0; b < BAND_COUNT; b++) {
        tile_t *t = &s_tile[b];
        const detect_status_t *st = &s->st[b];
        sweep_view_t v = {0}; /* no buffers: metadata only */
        sweep_get_view((band_id_t)b, &v);
        if (s->sweep != SWEEP_RUNNING) {
            colour_tile(t, DETECT_LEARNING);
            ui_set_text(t->state, s->sweep == SWEEP_FAILED ? "SWEEP FAILED" : "auto-ranging");
            ui_set_text(t->why, s->sweep == SWEEP_FAILED ? "see the serial log" : "choosing the receiver gain");
        } else {
            colour_tile(t, st->state);
            static const char *const words[] = {"LEARNING", "NORMAL", "SUSPECT", "INTERFERENCE"};
            if (st->state == DETECT_LEARNING) {
                snprintf(text, sizeof(text), "LEARNING %u%%", st->learn_pct);
                ui_set_text(t->state, text);
            } else {
                ui_set_text(t->state, words[st->state]);
            }
            why_text(text, sizeof(text), st, s->now_ms);
            ui_set_text(t->why, text);
        }
        const bool live = s->sweep == SWEEP_RUNNING && st->state != DETECT_LEARNING;
        if (live) {
            snprintf(text, sizeof(text), "floor %+.1f dB, cont. %+.1f dB", st->m.floor_delta_cdb / 100.0f,
                     st->m.cont_delta_cdb / 100.0f);
        } else {
            snprintf(text, sizeof(text), "floor -");
        }
        ui_set_text(t->floor, text);
        const unsigned duty = live ? st->m.evidence_duty_pct : 0;
        snprintf(text, sizeof(text), "evidence duty %u%%", duty);
        ui_set_text(t->duty, text);
        if (lv_bar_get_value(t->duty_bar) != (int32_t)duty) {
            lv_bar_set_value(t->duty_bar, (int32_t)duty, LV_ANIM_OFF);
        }
        if (st->alarms) {
            ui_fmt_duration(d, sizeof(d), s->now_ms - st->last_alarm_ms);
            snprintf(text, sizeof(text), "last alarm: %s\n%s ago, %u since boot", detect_kind_name(st->last_alarm_kind),
                     d, (unsigned)st->alarms);
        } else {
            snprintf(text, sizeof(text), "no alarm since boot");
        }
        ui_set_text(t->last, text);
        snprintf(text, sizeof(text), "%.1f sweeps/s, gain %u", v.sweeps_per_10s / 10.0f, v.gain);
        ui_set_text(t->rate, text);
    }
}
