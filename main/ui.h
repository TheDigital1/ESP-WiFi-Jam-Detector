/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LVGL screens: status (default), spectrum and settings. BOOT cycles the
 * screens; a long press (1.5 s) re-learns the baselines. Any ALARM flashes a
 * red border on every screen.
 */
#pragma once

#include "esp_err.h"

/* lvp_init(), settings_init(), sweep_start() and detector_start() must have run. */
esp_err_t ui_init(void);

/* Change one setting from outside the UI (the USB link): key as in params.h,
 * value checked against the slider's range. Applies, saves and updates the
 * settings screen. Takes the LVGL lock; never call from LVGL callbacks.
 * ESP_ERR_INVALID_ARG for an unknown key or a bad value. */
esp_err_t ui_settings_set_key(const char *key, int value);
