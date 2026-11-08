/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The user-adjustable settings as a table: name, key, unit and range of each
 * slider, plus the alarm beep and bands scanned. Shared by the settings
 * screen (ui_settings.c) and the USB link (link.c), so both offer the same
 * values. No IDF calls (host-tested in test_host/test_link.c).
 */
#pragma once

#include <stdbool.h>
#include "settings.h"

typedef enum {
    PARAM_LEARN,
    PARAM_WIDE_DB,
    PARAM_WIDE_PCT,
    PARAM_CW_DB,
    PARAM_CW_PCT,
    PARAM_SWEPT_DB,
    PARAM_SWEPT_PCT,
    PARAM_DUTY,
    PARAM_HOLD,
    PARAM_CLEAR,
    PARAM_WEIGHT,
    PARAM_BRIGHT,
    PARAM_COUNT
} param_t;

typedef struct {
    const char *name, *key, *unit;
    int min, max, step; /* tighter than detect_check_config(): only sensible values */
} param_info_t;

/* Keys of the two settings that are not sliders. */
#define PARAM_KEY_BEEP "beep" /* 0 / 1 */
#define PARAM_KEY_SCAN "scan" /* bit 0: 2.4 GHz, bit 1: 5 GHz; at least one */

extern const param_info_t param_info[PARAM_COUNT];

int param_get(const settings_t *s, param_t p);
void param_set(settings_t *s, param_t p, int v);
/* Nearest step, clamped to the range. */
int param_clamp(param_t p, int v);
/* PARAM_COUNT if no slider has this key. */
param_t param_find(const char *key);

/* Sets one value by key (sliders, beep, scan). -1 for an unknown key or a
 * value out of range or off the step; s is unchanged then. */
int param_set_key(settings_t *s, const char *key, int v);
int param_scan_mask(const settings_t *s);
