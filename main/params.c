/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "params.h"

const param_info_t param_info[PARAM_COUNT] = {
    [PARAM_LEARN] = {"Learn time", "learn_s", "s", 10, 300, 10},
    [PARAM_WIDE_DB] = {"Wideband rise", "wide_db", "dB", 3, 30, 1},
    [PARAM_WIDE_PCT] = {"Wideband share", "wide_pct", "%", 20, 100, 5},
    [PARAM_CW_DB] = {"Narrowband rise", "cw_db", "dB", 6, 40, 1},
    [PARAM_CW_PCT] = {"Narrowband persist", "cw_pct", "%", 50, 100, 5},
    [PARAM_SWEPT_DB] = {"Swept rise", "swept_db", "dB", 6, 40, 1},
    [PARAM_SWEPT_PCT] = {"Swept coverage", "swept_pct", "%", 20, 100, 5},
    [PARAM_DUTY] = {"Alarm duty", "duty_pct", "%", 30, 100, 5},
    [PARAM_HOLD] = {"Alarm after", "hold_s", "s", 2, 20, 1},
    [PARAM_CLEAR] = {"Clear after", "clear_s", "s", 3, 60, 1},
    [PARAM_WEIGHT] = {"2.4 sweeps per 5 GHz", "weight", "", 1, 6, 1},
    [PARAM_BRIGHT] = {"Brightness", "brightness", "%", 10, 100, 5},
};

int param_get(const settings_t *s, param_t p)
{
    const detect_config_t *c = &s->det;
    switch (p) {
    case PARAM_LEARN: return c->learn_s;
    case PARAM_WIDE_DB: return c->wide_cdb / 100;
    case PARAM_WIDE_PCT: return c->wide_pct;
    case PARAM_CW_DB: return c->cw_cdb / 100;
    case PARAM_CW_PCT: return c->cw_pct;
    case PARAM_SWEPT_DB: return c->swept_cdb / 100;
    case PARAM_SWEPT_PCT: return c->swept_cover_pct;
    case PARAM_DUTY: return c->duty_pct;
    case PARAM_HOLD: return c->hold_s;
    case PARAM_CLEAR: return c->clear_s;
    case PARAM_WEIGHT: return s->weight[BAND_24];
    case PARAM_BRIGHT: return s->brightness;
    default: return 0;
    }
}

void param_set(settings_t *s, param_t p, int v)
{
    detect_config_t *c = &s->det;
    switch (p) {
    case PARAM_LEARN: c->learn_s = (uint16_t)v; break;
    case PARAM_WIDE_DB: c->wide_cdb = (int16_t)(v * 100); break;
    case PARAM_WIDE_PCT: c->wide_pct = (uint8_t)v; break;
    case PARAM_CW_DB: c->cw_cdb = (int16_t)(v * 100); break;
    case PARAM_CW_PCT: c->cw_pct = (uint8_t)v; break;
    case PARAM_SWEPT_DB: c->swept_cdb = (int16_t)(v * 100); break;
    case PARAM_SWEPT_PCT: c->swept_cover_pct = (uint8_t)v; break;
    case PARAM_DUTY: c->duty_pct = (uint8_t)v; break;
    case PARAM_HOLD: c->hold_s = (uint16_t)v; break;
    case PARAM_CLEAR: c->clear_s = (uint16_t)v; break;
    case PARAM_WEIGHT: s->weight[BAND_24] = (uint8_t)v; s->weight[BAND_5] = 1; break;
    case PARAM_BRIGHT: s->brightness = (uint8_t)v; break;
    default: break;
    }
}

int param_clamp(param_t p, int v)
{
    const param_info_t *i = &param_info[p];
    v = (v - i->min + i->step / 2) / i->step * i->step + i->min;
    return v < i->min ? i->min : (v > i->max ? i->max : v);
}

param_t param_find(const char *key)
{
    for (int p = 0; p < PARAM_COUNT; p++) {
        if (!strcmp(param_info[p].key, key)) {
            return (param_t)p;
        }
    }
    return PARAM_COUNT;
}

int param_scan_mask(const settings_t *s)
{
    return (s->scan[BAND_24] ? 1 : 0) | (s->scan[BAND_5] ? 2 : 0);
}

int param_set_key(settings_t *s, const char *key, int v)
{
    if (!strcmp(key, PARAM_KEY_BEEP)) {
        if (v != 0 && v != 1) {
            return -1;
        }
        s->beep = v != 0;
        return 0;
    }
    if (!strcmp(key, PARAM_KEY_SCAN)) {
        if (v < 1 || v > 3) {
            return -1;
        }
        s->scan[BAND_24] = (v & 1) != 0;
        s->scan[BAND_5] = (v & 2) != 0;
        return 0;
    }
    const param_t p = param_find(key);
    if (p == PARAM_COUNT) {
        return -1;
    }
    const param_info_t *i = &param_info[p];
    if (v < i->min || v > i->max || (v - i->min) % i->step) {
        return -1;
    }
    param_set(s, p, v);
    return 0;
}
