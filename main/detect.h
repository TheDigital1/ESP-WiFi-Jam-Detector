/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Interference / possible-jamming detection for one band, fed one stitched
 * spectrum per completed sweep. The receiver is uncalibrated, so everything
 * is relative to a learned baseline. No ESP-IDF headers (host-tested).
 *
 * Inputs per grid bin (0.01 dB, from the sweep):
 *   avg: mean power over the K snapshots of the tune
 *   min: lowest power over the K snapshots (energy present in every snapshot)
 *   max: highest power over the K snapshots
 *
 * Evidence, evaluated every sweep:
 *   WIDEBAND   the 20th-percentile floor of the min spectrum (energy present
 *              in every snapshot) rose >= wide_cdb in >= wide_pct of the
 *              10 MHz cells
 *   PULSED     the same for the mean spectrum only, or >= sat_pct of the
 *              snapshots clipped: wideband energy that comes and goes. Busy
 *              Wi-Fi and 50/60 Hz-gated microwave ovens look like this, so it
 *              can raise SUSPECT but never ALARM.
 *   SWEPT      a narrow strong peak moved monotonically (sawtooth wraps
 *              allowed) with consistent steps and covered >= swept_cover_pct
 *              of the band
 *   NARROWBAND a cluster of <= cw_max_bins bins whose min power stayed
 *              >= cw_cdb above baseline in >= cw_pct of the window's sweeps,
 *              with quiet shoulders (a saturated Wi-Fi channel is wide and
 *              is deliberately not flagged)
 * State machine: NORMAL -> SUSPECT (any evidence in 2 consecutive sweeps) ->
 * ALARM (SUSPECT for >= hold_s and WIDEBAND/SWEPT/NARROWBAND evidence in
 * >= duty_pct of the sweeps of the last hold_s) -> NORMAL (none of those
 * for clear_s). SUSPECT falls back to NORMAL after hold_s without evidence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bandplan.h"

#define DETECT_CELL_BINS 32 /* 10 MHz detection cells at 312.5 kHz; the last cell takes the remainder */
#define DETECT_LEARN_MAX 32 /* baseline samples kept per bin (decimated over the learning time) */
#define DETECT_HIST      32 /* sweeps of per-bin / per-cell history */

typedef enum {
    DETECT_LEARNING,
    DETECT_NORMAL,
    DETECT_SUSPECT,
    DETECT_ALARM,
} detect_state_t;

/* Evidence kinds, strongest first when several are present. */
typedef enum {
    DETECT_NONE = 0,
    DETECT_WIDEBAND,
    DETECT_SWEPT,
    DETECT_NARROWBAND,
    DETECT_PULSED,
    DETECT_KIND_COUNT
} detect_kind_t;

#define DETECT_EV(kind)  (1u << (kind))
#define DETECT_EV_STRONG (DETECT_EV(DETECT_WIDEBAND) | DETECT_EV(DETECT_SWEPT) | DETECT_EV(DETECT_NARROWBAND))

typedef struct {
    uint16_t learn_s;        /* baseline learning time */
    uint8_t ema_shift;       /* after learning, baseline follows 1/2^shift of the (clipped) change per sweep */
    int16_t ema_clip_cdb;    /* largest change one sweep may contribute to the baseline */
    int16_t occ_cdb;         /* occupancy: bin this far above the cell's baseline floor */
    int16_t wide_cdb;        /* wideband / pulsed: cell floor rise */
    uint8_t wide_pct;        /* ... in at least this share of the band's cells */
    uint8_t sat_pct;         /* pulsed: or at least this share of snapshots clipped */
    int16_t cw_cdb;          /* narrowband: min power above baseline */
    uint8_t cw_pct;          /* ... in at least this share of the window's sweeps */
    uint8_t cw_max_bins;     /* ... in a cluster at most this wide */
    int16_t swept_cdb;       /* swept: peak above baseline */
    uint8_t swept_max_bins;  /* ... at most this wide at -6 dB */
    uint8_t swept_steps;     /* ... moving monotonically for this many sweeps */
    uint8_t swept_cover_pct; /* ... covering this share of the band */
    uint8_t duty_pct;        /* ALARM: evidence in this share of the window's sweeps */
    uint16_t hold_s;         /* window length, and minimum SUSPECT time before ALARM */
    uint16_t clear_s;        /* ALARM hold-off: no evidence this long -> NORMAL */
} detect_config_t;

/* Band-level metrics from the latest sweep. */
typedef struct {
    int16_t floor_delta_cdb;     /* median over cells of floor - baseline floor (mean spectrum) */
    int16_t cont_delta_cdb;      /* the same for the min spectrum (continuous energy) */
    int16_t floor_delta_max_cdb; /* largest cell rise */
    uint32_t floor_delta_max_khz;/* centre of that cell */
    int16_t flatness_cdb;        /* spectral flatness (geo/arith mean) of that cell, <= 0 */
    uint8_t wide_pct;            /* cells with floor rise >= wide_cdb */
    uint8_t cont_pct;            /* cells with min-spectrum floor rise >= wide_cdb */
    uint8_t occupancy_pct;       /* bins >= occ_cdb above their cell's baseline floor */
    uint8_t duty_pct;            /* largest per-cell share of window sweeps with floor rise >= wide_cdb */
    uint8_t clipped_pct;         /* snapshots with clipping */
    uint8_t evidence;            /* DETECT_EV() bits, this sweep */
    uint8_t evidence_duty_pct;   /* sweeps with ALARM-capable evidence, over the window */
    uint8_t window_sweeps;       /* sweeps in the window */
    uint32_t cw_khz;             /* strongest narrowband cluster (if any) */
    uint16_t cw_bins;
    int16_t cw_excess_cdb;
    uint32_t swept_khz;          /* swept tracker: last peak, speed, steps and cover */
    int32_t swept_khz_per_s;
    uint8_t swept_steps;
    uint8_t swept_cover_pct;
} detect_metrics_t;

typedef struct {
    detect_state_t state;
    detect_kind_t kind;     /* reason for SUSPECT / ALARM */
    uint8_t learn_pct;      /* progress while LEARNING */
    uint32_t sweeps;        /* since init */
    uint32_t state_since_ms;
    uint32_t last_alarm_ms; /* start of the latest ALARM */
    uint32_t alarms;        /* ALARM entries since init */
    detect_kind_t last_alarm_kind;
    detect_metrics_t m;
} detect_status_t;

typedef struct {
    detect_state_t from, to;
    detect_kind_t kind;
    uint32_t t_ms;
    detect_metrics_t m;
} detect_event_t;

typedef struct {
    const int16_t *avg_cdb, *min_cdb, *max_cdb; /* n_bins each */
    unsigned clipped_snapshots, snapshots;      /* over the whole sweep */
    unsigned gain;                              /* a gain change restarts learning */
    uint32_t t_start_ms, t_ms;                  /* sweep start and end, monotonic */
} detect_input_t;

typedef struct {
    detect_config_t cfg;
    const band_plan_t *plan;
    unsigned n_bins, n_cells;
    /* Working memory, carved from the caller's block. */
    int32_t *base_q8;     /* per-bin baseline (mean spectrum), cdB * 256 */
    int32_t *cell_q8[2];  /* per-cell baseline floor: [0] mean, [1] min spectrum */
    uint32_t *cw_hist;    /* per bin: bit i = min >= baseline + cw_cdb, i sweeps ago */
    uint32_t *cell_hist;  /* per cell: bit i = mean floor rise >= wide_cdb */
    int16_t *learn_bin;   /* n_bins * DETECT_LEARN_MAX */
    int16_t *learn_cell[2];
    int16_t *cell_floor[2]; /* this sweep */
    int16_t *cell_delta[2];
    int16_t *scratch;     /* n_bins */
    uint8_t *mark;        /* n_bins */
    uint32_t t_hist[DETECT_HIST]; /* sweep times, newest at t_head */
    unsigned t_head, t_count;
    uint32_t ev_hist, strong_hist; /* any / ALARM-capable evidence per sweep */
    uint32_t last_strong_ms;
    /* learning */
    unsigned learn_n, learn_stride, learn_seen;
    uint32_t learn_start_ms;
    bool learn_started;
    unsigned gain;
    /* state machine */
    detect_status_t st;
    uint32_t last_ev_ms, suspect_since_ms;
    /* swept tracker */
    int sw_pos, sw_dir;
    uint32_t sw_t, sw_run_ms;
    unsigned sw_steps, sw_cover, sw_misses;
} detect_t;

void detect_default_config(detect_config_t *cfg);
/* 0 if every field is in range, else -1. */
int detect_check_config(const detect_config_t *cfg);

/* Bytes of working memory detect_init() needs for this plan. */
size_t detect_mem_bytes(const band_plan_t *plan);
/* mem must be 4-byte aligned and hold detect_mem_bytes(plan). The plan must
 * stay valid. Starts in LEARNING. Returns 0, or -1 on bad arguments. */
int detect_init(detect_t *d, const band_plan_t *plan, const detect_config_t *cfg, void *mem, size_t mem_bytes);
/* New thresholds; keeps the baseline (re-learn separately if wanted). */
int detect_set_config(detect_t *d, const detect_config_t *cfg);
/* Forget the baseline and learn again from the next sweep. */
void detect_relearn(detect_t *d);

/* Process one sweep. Returns true and fills *ev on a state change. */
bool detect_sweep(detect_t *d, const detect_input_t *in, detect_event_t *ev);

void detect_get_status(const detect_t *d, detect_status_t *out);
/* Per-bin baseline in cdB (meaningless while LEARNING). */
void detect_get_baseline(const detect_t *d, int16_t *out_cdb);

/* Learned baseline as a self-describing record (plan geometry and gain), so
 * it can be stored and restored across reboots. */
size_t detect_baseline_bytes(const band_plan_t *plan);
/* Fills buf; returns the record size, or 0 while LEARNING or if len is short. */
size_t detect_save_baseline(const detect_t *d, void *buf, size_t len);
/* Restores a record from detect_save_baseline() for the same plan and goes
 * to NORMAL; a later sweep at a different gain re-learns as usual.
 * Returns 0, or -1 if the record does not match (state unchanged). */
int detect_load_baseline(detect_t *d, const void *buf, size_t len);

const char *detect_state_name(detect_state_t s);
const char *detect_kind_name(detect_kind_t k);
