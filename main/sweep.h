/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sweep task: tunes through each band's plan, takes K snapshots per tune,
 * builds average and per-snapshot-max spectra, and stitches them into one
 * grid per band. The only task that touches the receiver.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bandplan.h"
#include "espsdr_rx.h"

#define SWEEP_GAIN_AUTO 0xFFu /* auto-range this band when the sweep starts */

typedef struct {
    rx_rate_t rate;
    unsigned bw_mhz;              /* 0 = widest analog filter */
    uint32_t usable_hz;           /* centre span kept per capture */
    unsigned snapshots;           /* K per tune */
    unsigned weight[BAND_COUNT];  /* sweeps of each band per schedule round */
    unsigned gain[BAND_COUNT];    /* manual gain index, or SWEEP_GAIN_AUTO */
    unsigned yield_pct;           /* CPU share left to other tasks (UI, detect, idle), 0..90 */
} sweep_config_t;

/* One tune's result, passed to the segment callback (M4: detector). */
typedef struct {
    band_id_t band;
    unsigned slice;
    unsigned center_mhz;
    unsigned first_bin, bins; /* position in the band grid */
    const int16_t *avg_cdb;   /* bins entries, 0.01 dB */
    const int16_t *max_cdb;   /* per-snapshot max over the K snapshots */
    unsigned gain;
    unsigned snapshots, clipped_snapshots;
    int64_t t_us;
    bool last_in_sweep;
} sweep_segment_t;

typedef void (*sweep_segment_cb_t)(const sweep_segment_t *seg, void *ctx);

typedef enum {
    SWEEP_IDLE,
    SWEEP_AUTORANGE,
    SWEEP_RUNNING,
    SWEEP_FAILED,
} sweep_state_t;

/* Copy of one band's stitched spectrum for the UI. */
typedef struct {
    unsigned n_bins;
    int16_t *avg_cdb, *max_cdb, *hold_cdb; /* caller buffers, n_bins each */
    uint32_t sweeps;
    uint32_t last_sweep_ms;
    uint32_t sweeps_per_10s; /* sweeps completed in the last 10 s window */
    unsigned gain;
    unsigned clipped_snapshots; /* in the last sweep */
    unsigned snapshots_per_sweep;
    bool valid;                 /* at least one full sweep done */
} sweep_view_t;

/* Build plans and start the sweep task (auto-range runs first, in the task). */
esp_err_t sweep_start(const sweep_config_t *cfg);
sweep_state_t sweep_state(void);
const band_plan_t *sweep_plan(band_id_t band);

/* Copy the latest stitched spectra; out->*_cdb must hold n_bins entries. */
esp_err_t sweep_get_view(band_id_t band, sweep_view_t *out);
void sweep_reset_maxhold(band_id_t band);

/* Called from the sweep task after every tune. Keep it short; copy and queue. */
void sweep_set_segment_cb(sweep_segment_cb_t cb, void *ctx);
