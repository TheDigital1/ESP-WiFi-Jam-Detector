/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "esp_err.h"

/* Filter-shape and LCD self-interference measurements, logged only.
 * board_init() and rx_init() must have run; LVGL must not be running yet. */
esp_err_t characterize_run(void);
