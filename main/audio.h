/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Alarm beeps through the ES8311 codec and NS4150B amplifier. The codec,
 * I2S and amplifier only run while a beep plays.
 */
#pragma once

#include "esp_err.h"

/* board_init() must have run. */
esp_err_t audio_init(void);
/* Queue count beeps of on_ms each, off_ms apart; returns at once. Dropped if
 * another pattern is already waiting. */
void audio_beep(unsigned count, unsigned on_ms, unsigned off_ms);
