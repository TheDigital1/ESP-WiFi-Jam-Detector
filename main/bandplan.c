/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include "bandplan.h"

/* Wi-Fi ranges per the project brief, section 5. 5350-5470 MHz has no U-NII
 * label but is still swept so the spectrum is continuous. */
static const bp_label_t s_labels_5[] = {
    {"U-NII-1", 5150000, 5250000},
    {"U-NII-2A", 5250000, 5350000},
    {"U-NII-2C", 5470000, 5730000},
    {"U-NII-3", 5735000, 5895000},
};

static const bp_tick_t s_ticks_24[] = {
    {"1", 2412000},
    {"6", 2437000},
    {"11", 2462000},
};

static const bp_tick_t s_ticks_5[] = {
    {"36", 5180000},
    {"64", 5320000},
    {"100", 5500000},
    {"140", 5700000},
    {"165", 5825000},
};

int bp_build(band_plan_t *p, band_id_t band, const bp_config_t *cfg)
{
    if (!cfg->fft_n || !cfg->decim || cfg->usable_hz % 1000000u || cfg->usable_hz >= cfg->rate_hz) {
        return -1;
    }
    const uint64_t bin_hz_x_n = cfg->rate_hz; /* bin width = rate / n, kept exact as rate/n */
    if ((uint64_t)cfg->decim * bin_hz_x_n % cfg->fft_n) {
        return -1; /* grid resolution must be whole Hz */
    }

    p->id = band;
    if (band == BAND_24) {
        p->name = "2.4 GHz";
        p->start_khz = 2400000;
        p->stop_khz = 2483500;
        p->labels = NULL;
        p->n_labels = 0;
        p->ticks = s_ticks_24;
        p->n_ticks = sizeof(s_ticks_24) / sizeof(s_ticks_24[0]);
    } else if (band == BAND_5) {
        p->name = "5 GHz";
        p->start_khz = 5150000;
        p->stop_khz = 5895000;
        p->labels = s_labels_5;
        p->n_labels = sizeof(s_labels_5) / sizeof(s_labels_5[0]);
        p->ticks = s_ticks_5;
        p->n_ticks = sizeof(s_ticks_5) / sizeof(s_ticks_5[0]);
    } else {
        return -1;
    }
    p->decim = cfg->decim;
    p->fft_n = cfg->fft_n;
    p->res_hz = (uint32_t)((uint64_t)cfg->decim * cfg->rate_hz / cfg->fft_n);

    const uint64_t span_hz = (uint64_t)(p->stop_khz - p->start_khz) * 1000u;
    const uint32_t step_hz = cfg->usable_hz;
    p->n_slices = (unsigned)((span_hz + step_hz - 1) / step_hz);
    p->n_bins = (unsigned)((span_hz + p->res_hz - 1) / p->res_hz);
    if (p->n_slices > BP_MAX_SLICES || p->n_bins > BP_MAX_BINS) {
        return -1;
    }

    const uint64_t start_hz = (uint64_t)p->start_khz * 1000u;
    for (unsigned s = 0; s < p->n_slices; s++) {
        p->center_mhz[s] = (uint16_t)((start_hz + step_hz / 2 + (uint64_t)s * step_hz) / 1000000u);
        p->slice_bins[s] = 0;
    }
    /* Each grid bin belongs to the slice whose step contains its centre. */
    int prev = -1;
    for (unsigned k = 0; k < p->n_bins; k++) {
        uint64_t centre_hz = start_hz + (uint64_t)k * p->res_hz + p->res_hz / 2;
        unsigned s = (unsigned)((centre_hz - start_hz) / step_hz);
        if (s >= p->n_slices) {
            s = p->n_slices - 1;
        }
        if ((int)s != prev) {
            p->slice_first_bin[s] = (uint16_t)k;
            /* FFT bin holding the start of grid bin k: n/2 + (f - centre) / (rate/n), rounded. */
            int64_t off_hz = (int64_t)(start_hz + (uint64_t)k * p->res_hz) - (int64_t)p->center_mhz[s] * 1000000;
            int64_t num = off_hz * (int64_t)cfg->fft_n;
            int64_t den = (int64_t)cfg->rate_hz;
            int64_t idx = (int64_t)cfg->fft_n / 2 + (num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den));
            if (idx < 0) {
                return -1;
            }
            p->slice_fft_first[s] = (uint16_t)idx;
            prev = (int)s;
        }
        p->slice_bins[s]++;
    }
    for (unsigned s = 0; s < p->n_slices; s++) {
        if (!p->slice_bins[s] || p->slice_fft_first[s] + (unsigned)p->slice_bins[s] * cfg->decim > cfg->fft_n) {
            return -1;
        }
    }
    return 0;
}

int bp_label_of(const band_plan_t *p, uint32_t khz)
{
    for (unsigned i = 0; i < p->n_labels; i++) {
        if (khz >= p->labels[i].lo_khz && khz < p->labels[i].hi_khz) {
            return (int)i;
        }
    }
    return -1;
}
