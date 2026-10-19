/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LVGL 9 glue for the board: display flush through board_lcd_draw_async()
 * (so LCD DMA respects the sweep's radio-quiet windows), FT6336 touch input,
 * tick, a handler task and a lock. LVGL's heap is in PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* board_init() must have run. Starts the LVGL task. */
esp_err_t lvp_init(void);

/* Hold this around any LVGL call made outside LVGL callbacks/timers. */
bool lvp_lock(uint32_t timeout_ms);
void lvp_unlock(void);
