/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared between the UI screens (ui.c, ui_status.c, ui_spectrum.c,
 * ui_settings.c). Everything here runs in the LVGL task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "bandplan.h"
#include "board.h"
#include "detect.h"
#include "sweep.h"

typedef enum {
    UI_STATUS,
    UI_SPECTRUM,
    UI_SETTINGS,
    UI_SCREEN_COUNT
} ui_screen_t;

/* Polled once per refresh tick and handed to the visible screen. */
typedef struct {
    detect_status_t st[BAND_COUNT];
    sweep_state_t sweep;
    uint32_t now_ms; /* same clock as the detector's times */
    board_power_t power;
    bool power_ok;
} ui_snap_t;

void ui_show(ui_screen_t screen);
void ui_show_spectrum(band_id_t band);

/* Helpers */
lv_obj_t *ui_screen_create(void);
lv_color_t ui_state_colour(detect_state_t s);
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_colour(lv_obj_t *label, lv_color_t c);
/* "learning baseline 40%", "normal", "suspect: swept", "INTERFERENCE: wideband" */
void ui_state_text(char *buf, size_t len, const detect_status_t *st);
/* "1:02:03" or "12:03" */
void ui_fmt_duration(char *buf, size_t len, uint32_t ms);

/* Screens: create once; enter when shown; update every tick while shown. */
lv_obj_t *ui_status_create(void);
void ui_status_update(const ui_snap_t *s);

lv_obj_t *ui_spectrum_create(void);
void ui_spectrum_enter(band_id_t band);
void ui_spectrum_leave(void);
/* Every tick, whatever is shown: keeps the waterfall history. */
void ui_spectrum_feed(const ui_snap_t *s);
void ui_spectrum_update(const ui_snap_t *s);

lv_obj_t *ui_settings_create(void);
void ui_settings_enter(void);
void ui_settings_leave(void);
void ui_settings_update(const ui_snap_t *s);
