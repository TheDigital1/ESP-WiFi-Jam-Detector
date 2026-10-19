/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LVGL screens. M3: one live spectrum screen (average + max-hold) per band.
 */
#pragma once

#include "esp_err.h"

/* lvp_init() and sweep_start() must have run. */
esp_err_t ui_init(void);
