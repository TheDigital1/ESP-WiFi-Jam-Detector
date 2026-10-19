/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Band plan: tune centres that tile each band, and the mapping from each
 * slice's FFT bins onto one stitched per-band frequency grid. No ESP-IDF
 * headers (host-tested).
 */
#pragma once

#include <stdint.h>

#define BP_MAX_SLICES 32
#define BP_MAX_BINS   4096

typedef enum {
    BAND_24 = 0,
    BAND_5 = 1,
    BAND_COUNT
} band_id_t;

typedef struct {
    const char *name;
    uint32_t lo_khz, hi_khz;
} bp_label_t;

typedef struct {
    const char *name;
    uint32_t khz;
} bp_tick_t;

typedef struct {
    uint32_t rate_hz;   /* complex sample rate */
    unsigned fft_n;     /* FFT size */
    uint32_t usable_hz; /* centre span kept per capture; must be whole MHz (tune step) */
    unsigned decim;     /* FFT bins per grid bin */
} bp_config_t;

typedef struct {
    band_id_t id;
    const char *name;
    uint32_t start_khz, stop_khz;
    uint32_t res_hz; /* grid bin width = decim * rate / fft_n */
    unsigned decim, fft_n;
    unsigned n_bins; /* grid bins covering start..stop */
    unsigned n_slices;
    uint16_t center_mhz[BP_MAX_SLICES];
    uint16_t slice_first_bin[BP_MAX_SLICES]; /* first grid bin owned by the slice */
    uint16_t slice_bins[BP_MAX_SLICES];      /* grid bins owned by the slice */
    uint16_t slice_fft_first[BP_MAX_SLICES]; /* FFT bin (DC-centred order) of slice_first_bin */
    const bp_label_t *labels;                /* sub-bands (UNII) */
    unsigned n_labels;
    const bp_tick_t *ticks; /* channel ticks for the UI */
    unsigned n_ticks;
} band_plan_t;

/* Build the plan for one band. Returns 0, or -1 if the config does not fit. */
int bp_build(band_plan_t *p, band_id_t band, const bp_config_t *cfg);

/* Frequency (kHz) at the start of grid bin k. */
static inline uint32_t bp_bin_khz(const band_plan_t *p, unsigned k)
{
    return p->start_khz + (uint32_t)(((uint64_t)k * p->res_hz) / 1000u);
}

/* Index of the sub-band label containing khz, or -1. */
int bp_label_of(const band_plan_t *p, uint32_t khz);
