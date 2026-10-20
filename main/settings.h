/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Persistent settings (thresholds, sweep weighting, auto-range gains, beep,
 * brightness) and learned baselines, in the "jd_nvs" partition.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "bandplan.h"
#include "detect.h"

#define SETTINGS_GAIN_NONE 0xFFu /* not auto-ranged yet */

typedef struct {
    detect_config_t det;
    uint8_t weight[BAND_COUNT]; /* sweeps of each band per schedule round */
    uint8_t gain[BAND_COUNT];   /* last auto-range result, or SETTINGS_GAIN_NONE */
    bool beep;                  /* alarm beep (M6) */
    uint8_t brightness;         /* backlight, percent */
} settings_t;

void settings_default(settings_t *s);
/* 0 if every field is in range, else -1. */
int settings_check(const settings_t *s);

/* Opens the partition and loads the settings (defaults if none or invalid). */
esp_err_t settings_init(void);
void settings_get(settings_t *out);
/* Validates, keeps and writes to flash (skipped when unchanged). */
esp_err_t settings_set(const settings_t *s);

/* Learned baseline records (detect_save_baseline()), one per band. */
esp_err_t settings_save_baseline(band_id_t band, const void *rec, size_t len);
/* ESP_ERR_NOT_FOUND if none; *len gets the record size. */
esp_err_t settings_load_baseline(band_id_t band, void *rec, size_t cap, size_t *len);
esp_err_t settings_erase_baselines(void);
