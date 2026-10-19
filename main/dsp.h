/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fixed-point spectrum helpers (the ESP32-C5 has no FPU). No ESP-IDF headers:
 * this file also builds on the host for unit tests.
 */
#pragma once

#include <stdint.h>

#define DSP_MAX_N 2048

typedef struct {
    unsigned n, log2n;
    int16_t *window;  /* n entries, Q15 Hann */
    int16_t *twiddle; /* n entries (n/2 complex), bit-reversed, esp-dsp layout */
    int16_t *work;    /* 2n entries, interleaved complex */
} dsp_fft_t;

/* Caller supplies all buffers (allocate once, e.g. in PSRAM). n must be a
 * power of two, 256..DSP_MAX_N. Returns 0 on success, -1 on bad n. */
int dsp_fft_init(dsp_fft_t *f, unsigned n, int16_t *window, int16_t *twiddle, int16_t *work);

/* Hann window + Q15 FFT of n interleaved 10-bit I/Q samples. Writes n bin
 * powers in natural, DC-centred order: power[0] is -fs/2, power[n/2] is DC.
 * The FFT scales by 1/n, so a full-scale tone is about 2^28. */
void dsp_power_spectrum(dsp_fft_t *f, const int16_t *iq, uint32_t *power);

/* 10*log10(p) in 0.01 dB units; 0 for p == 0. Error < 0.03 dB. */
int16_t dsp_cdb(uint32_t p);

/* Convert powers to cdB for bins [lo, hi). */
void dsp_to_cdb(const uint32_t *power, int16_t *cdb, unsigned lo, unsigned hi);

/* pct-th percentile (0..100) of cdb[lo, hi), skipping bins with
 * |j - dc| < dc_guard (0 skips nothing, 1 skips only DC, 3 skips DC +/- 2).
 * scratch needs hi - lo entries. */
int16_t dsp_percentile(const int16_t *cdb, unsigned lo, unsigned hi, unsigned dc, unsigned dc_guard,
                       unsigned pct, int16_t *scratch);

/* Index of the strongest bin in [lo, hi), skipping the DC guard. */
unsigned dsp_peak_bin(const int16_t *cdb, unsigned lo, unsigned hi, unsigned dc, unsigned dc_guard);
