/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See detect.h for the method. Integer only (the ESP32-C5 has no FPU).
 */
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "detect.h"

#define LEARN_MIN      4  /* samples per bin before the baseline may be used */
#define SHOULDER_GAP   3  /* narrowband: bins skipped beside a cluster (window leakage) */
#define SHOULDER_BINS  8  /* ... then this many bins checked on each side */
#define SWEPT_MIN_STEP 2  /* bins per sweep; slower moves count as stationary */
#define PEAK_WIDTH_CDB 600
#define PEAK_TRIES     4  /* swept: wide peaks blanked before giving up */
#define MARK_PERSIST   1u
#define MARK_BLANK     2u

/* 2^30 * 10^(-i/100): i in tenths of a dB. */
static const uint32_t s_db_q30[100] = {
    1073741824u, 1049300476u, 1025415481u, 1002074175u, 979264182u, 956973408u, 935190034u, 913902510u,
    893099549u, 872770121u, 852903448u, 833488995u, 814516469u, 795975811u, 777857189u, 760150998u,
    742847849u, 725938567u, 709414188u, 693265950u, 677485290u, 662063842u, 646993429u, 632266061u,
    617873928u, 603809400u, 590065020u, 576633501u, 563507720u, 550680718u, 538145694u, 525896003u,
    513925148u, 502226784u, 490794706u, 479622855u, 468705306u, 458036271u, 447610092u, 437421243u,
    427464319u, 417734044u, 408225256u, 398932915u, 389852093u, 380977976u, 372305858u, 363831142u,
    355549334u, 347456043u, 339546978u, 331817945u, 324264847u, 316883678u, 309670525u, 302621563u,
    295733055u, 289001349u, 282422875u, 275994146u, 269711752u, 263572363u, 257572723u, 251709652u,
    245980041u, 240380852u, 234909116u, 229561931u, 224336464u, 219229942u, 214239660u, 209362970u,
    204597287u, 199940084u, 195388892u, 190941298u, 186594943u, 182347524u, 178196787u, 174140533u,
    170176611u, 166302918u, 162517402u, 158818054u, 155202914u, 151670064u, 148217632u, 144843787u,
    141546739u, 138324742u, 135176087u, 132099103u, 129092161u, 126153664u, 123282056u, 120475814u,
    117733450u, 115053509u, 112434572u, 109875248u,
};

/* Linear power, 2^30 = the reference, for a level below_cdb under it. */
static uint32_t lin_q30(int32_t below_cdb)
{
    static const uint32_t p10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (below_cdb < 0) {
        below_cdb = 0;
    }
    if (below_cdb >= 9000) {
        return 0;
    }
    return s_db_q30[(below_cdb % 1000) / 10] / p10[below_cdb / 1000];
}

static inline int16_t q8_cdb(int32_t q8)
{
    return (int16_t)((q8 + 128) >> 8);
}

static inline unsigned pct_of(unsigned part, unsigned whole)
{
    return whole ? part * 100u / whole : 0;
}

void detect_default_config(detect_config_t *cfg)
{
    *cfg = (detect_config_t){
        .learn_s = 60,
        .ema_shift = 5,
        .ema_clip_cdb = 100,
        .occ_cdb = 600,
        .wide_cdb = 1000,
        .wide_pct = 60,
        .sat_pct = 50,
        .cw_cdb = 1500,
        .cw_pct = 90,
        .cw_max_bins = 16,
        .swept_cdb = 1500,
        .swept_max_bins = 16,
        .swept_steps = 4,
        .swept_cover_pct = 60,
        .duty_pct = 80,
        .hold_s = 5,
        .clear_s = 10,
    };
}

#define IN(v, lo, hi) ((v) >= (lo) && (v) <= (hi))

int detect_check_config(const detect_config_t *c)
{
    bool ok = IN(c->learn_s, 5, 600) && IN(c->ema_shift, 1, 12) && IN(c->ema_clip_cdb, 0, 1000) &&
              IN(c->occ_cdb, 100, 3000) && IN(c->wide_cdb, 200, 4000) && IN(c->wide_pct, 1, 100) &&
              IN(c->sat_pct, 1, 100) && IN(c->cw_cdb, 300, 5000) && IN(c->cw_pct, 1, 100) &&
              IN(c->cw_max_bins, 1, 64) && IN(c->swept_cdb, 300, 5000) && IN(c->swept_max_bins, 1, 64) &&
              IN(c->swept_steps, 2, 30) && IN(c->swept_cover_pct, 1, 100) && IN(c->duty_pct, 1, 100) &&
              IN(c->hold_s, 1, 60) && IN(c->clear_s, 1, 600);
    return ok ? 0 : -1;
}

static unsigned cells_of(unsigned n_bins)
{
    unsigned n = n_bins / DETECT_CELL_BINS;
    return n ? n : 1;
}

static void cell_range(const detect_t *d, unsigned c, unsigned *lo, unsigned *hi)
{
    *lo = c * DETECT_CELL_BINS;
    *hi = c + 1 == d->n_cells ? d->n_bins : *lo + DETECT_CELL_BINS;
}

/* Carves the working arrays out of mem (or only sizes them when mem is NULL). */
static size_t layout(detect_t *d, unsigned nb, unsigned nc, uint8_t *mem)
{
    size_t used = 0;
#define CARVE(ptr, count)                                                     \
    do {                                                                      \
        (ptr) = mem ? (void *)(mem + used) : NULL;                            \
        used += ((size_t)(count) * sizeof(*(ptr)) + 3u) & ~(size_t)3u;        \
    } while (0)
    CARVE(d->base_q8, nb);
    CARVE(d->cw_hist, nb);
    CARVE(d->cell_hist, nc);
    CARVE(d->learn_bin, (size_t)nb * DETECT_LEARN_MAX);
    CARVE(d->scratch, nb);
    CARVE(d->mark, nb);
    for (int i = 0; i < 2; i++) {
        CARVE(d->cell_q8[i], nc);
        CARVE(d->learn_cell[i], (size_t)nc * DETECT_LEARN_MAX);
        CARVE(d->cell_floor[i], nc);
        CARVE(d->cell_delta[i], nc);
    }
#undef CARVE
    return used;
}

size_t detect_mem_bytes(const band_plan_t *plan)
{
    detect_t tmp;
    return layout(&tmp, plan->n_bins, cells_of(plan->n_bins), NULL);
}

int detect_init(detect_t *d, const band_plan_t *plan, const detect_config_t *cfg, void *mem, size_t mem_bytes)
{
    if (!d || !plan || !plan->n_bins || !cfg || !mem || ((uintptr_t)mem & 3u) ||
        mem_bytes < detect_mem_bytes(plan) || detect_check_config(cfg)) {
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    d->plan = plan;
    d->n_bins = plan->n_bins;
    d->n_cells = cells_of(plan->n_bins);
    memset(mem, 0, mem_bytes);
    layout(d, d->n_bins, d->n_cells, mem);
    detect_relearn(d);
    return 0;
}

int detect_set_config(detect_t *d, const detect_config_t *cfg)
{
    if (detect_check_config(cfg)) {
        return -1;
    }
    d->cfg = *cfg;
    return 0;
}

static void swept_reset(detect_t *d)
{
    d->sw_pos = -1;
    d->sw_dir = 0;
    d->sw_steps = d->sw_cover = d->sw_misses = 0;
    d->sw_run_ms = 0;
}

void detect_relearn(detect_t *d)
{
    d->learn_n = 0;
    d->learn_stride = 1;
    d->learn_seen = 0;
    d->learn_started = false;
    d->t_head = d->t_count = 0;
    d->ev_hist = d->strong_hist = 0;
    memset(d->cw_hist, 0, d->n_bins * sizeof(uint32_t));
    memset(d->cell_hist, 0, d->n_cells * sizeof(uint32_t));
    swept_reset(d);
    d->st.state = DETECT_LEARNING;
    d->st.kind = DETECT_NONE;
    d->st.learn_pct = 0;
    memset(&d->st.m, 0, sizeof(d->st.m));
}

static int16_t cell_floor_of(detect_t *d, const int16_t *cdb, unsigned c)
{
    unsigned lo, hi;
    cell_range(d, c, &lo, &hi);
    return dsp_percentile(cdb, lo, hi, 0, 0, 20, d->scratch);
}

static int16_t median_of(int16_t *v, unsigned n, int16_t *scratch)
{
    return dsp_percentile(v, 0, n, 0, 0, 50, scratch);
}

/* Returns true when learning finished with this sweep. */
static bool learn_step(detect_t *d, const detect_input_t *in)
{
    if (!d->learn_started) {
        d->learn_started = true;
        d->learn_start_ms = in->t_ms;
        d->gain = in->gain;
    }
    const int16_t *spec[2] = {in->avg_cdb, in->min_cdb};
    if (d->learn_seen++ % d->learn_stride == 0) {
        const unsigned i = d->learn_n++;
        for (unsigned k = 0; k < d->n_bins; k++) {
            d->learn_bin[k * DETECT_LEARN_MAX + i] = in->avg_cdb[k];
        }
        for (int s = 0; s < 2; s++) {
            for (unsigned c = 0; c < d->n_cells; c++) {
                d->learn_cell[s][c * DETECT_LEARN_MAX + i] = cell_floor_of(d, spec[s], c);
            }
        }
        if (d->learn_n == DETECT_LEARN_MAX) {
            /* Full: keep every other sample and sample half as often. */
            const unsigned rows = d->n_bins + 2 * d->n_cells;
            for (unsigned r = 0; r < rows; r++) {
                int16_t *row = r < d->n_bins ? &d->learn_bin[r * DETECT_LEARN_MAX]
                               : r < d->n_bins + d->n_cells
                                   ? &d->learn_cell[0][(r - d->n_bins) * DETECT_LEARN_MAX]
                                   : &d->learn_cell[1][(r - d->n_bins - d->n_cells) * DETECT_LEARN_MAX];
                for (unsigned j = 0; j < DETECT_LEARN_MAX / 2; j++) {
                    row[j] = row[2 * j];
                }
            }
            d->learn_n = DETECT_LEARN_MAX / 2;
            d->learn_stride *= 2;
        }
    }
    const uint32_t elapsed = in->t_ms - d->learn_start_ms, need = d->cfg.learn_s * 1000u;
    if (elapsed < need || d->learn_n < LEARN_MIN) {
        const uint32_t pct = (uint32_t)((uint64_t)elapsed * 100u / need);
        d->st.learn_pct = (uint8_t)(pct > 99 ? 99 : pct);
        return false;
    }
    d->st.learn_pct = 100;
    for (unsigned k = 0; k < d->n_bins; k++) {
        d->base_q8[k] = (int32_t)median_of(&d->learn_bin[k * DETECT_LEARN_MAX], d->learn_n, d->scratch) * 256;
    }
    for (int s = 0; s < 2; s++) {
        for (unsigned c = 0; c < d->n_cells; c++) {
            d->cell_q8[s][c] =
                (int32_t)median_of(&d->learn_cell[s][c * DETECT_LEARN_MAX], d->learn_n, d->scratch) * 256;
        }
    }
    return true;
}

/* Spectral flatness of cdb[lo, hi) in cdB: geometric over arithmetic mean
 * of linear power; 0 for a flat spectrum, very negative for a tone. */
static int16_t flatness(const int16_t *cdb, unsigned lo, unsigned hi)
{
    int16_t mx = INT16_MIN;
    int32_t sum_db = 0;
    for (unsigned k = lo; k < hi; k++) {
        mx = cdb[k] > mx ? cdb[k] : mx;
        sum_db += cdb[k];
    }
    uint64_t sum_lin = 0;
    for (unsigned k = lo; k < hi; k++) {
        sum_lin += lin_q30(mx - cdb[k]);
    }
    const int32_t n = (int32_t)(hi - lo);
    const int32_t geo = sum_db / n;
    const int32_t arith = mx + dsp_cdb((uint32_t)(sum_lin / (uint64_t)n)) - dsp_cdb(1u << 30);
    const int32_t f = geo - arith;
    return (int16_t)(f > 0 ? 0 : (f < INT16_MIN ? INT16_MIN : f));
}

static uint32_t bin_centre_khz(const detect_t *d, unsigned k)
{
    return bp_bin_khz(d->plan, k) + d->plan->res_hz / 2000u;
}

/* Narrowband clusters among the persistent bins (mark & MARK_PERSIST).
 * Returns true if one passed; fills the strongest into m. */
static bool find_narrowband(detect_t *d, const int16_t *min, detect_metrics_t *m)
{
    const unsigned n = d->n_bins;
    bool found = false;
    int32_t best = INT32_MIN;
    for (unsigned k = 0; k < n;) {
        if (!(d->mark[k] & MARK_PERSIST)) {
            k++;
            continue;
        }
        const unsigned a = k;
        unsigned b = k, j = k + 1;
        while (j < n) { /* one-bin gaps do not split a cluster */
            if (d->mark[j] & MARK_PERSIST) {
                b = j++;
            } else if (j + 1 < n && (d->mark[j + 1] & MARK_PERSIST)) {
                j++;
            } else {
                break;
            }
        }
        k = b + 1;
        const unsigned w = b - a + 1;
        if (w > d->cfg.cw_max_bins) {
            continue;
        }
        int32_t excess = INT32_MIN;
        unsigned peak = a;
        for (unsigned i = a; i <= b; i++) {
            int32_t e = min[i] - q8_cdb(d->base_q8[i]);
            if (e > excess) {
                excess = e;
                peak = i;
            }
        }
        /* Shoulders must be quiet: a fragment of a busy wide channel is not a tone. */
        unsigned total = 0, raised = 0;
        for (int side = -1; side <= 1; side += 2) {
            for (unsigned s = 0; s < SHOULDER_BINS; s++) {
                long i = side < 0 ? (long)a - SHOULDER_GAP - 1 - (long)s : (long)b + SHOULDER_GAP + 1 + (long)s;
                if (i < 0 || i >= (long)n) {
                    continue;
                }
                total++;
                raised += min[i] - q8_cdb(d->base_q8[i]) >= d->cfg.occ_cdb;
            }
        }
        if (2 * raised > total) {
            continue;
        }
        found = true;
        if (excess > best) {
            best = excess;
            m->cw_khz = bin_centre_khz(d, peak);
            m->cw_bins = (uint16_t)w;
            m->cw_excess_cdb = (int16_t)excess;
        }
    }
    return found;
}

/* Strongest narrow peak of avg above baseline, skipping persistent bins;
 * -1 if none reaches swept_cdb. */
static int find_swept_peak(detect_t *d, const int16_t *avg)
{
    const unsigned n = d->n_bins;
    for (int t = 0; t < PEAK_TRIES; t++) {
        int32_t e_max = INT32_MIN;
        unsigned k_max = 0;
        for (unsigned k = 0; k < n; k++) {
            if (d->mark[k]) {
                continue;
            }
            int32_t e = avg[k] - q8_cdb(d->base_q8[k]);
            if (e > e_max) {
                e_max = e;
                k_max = k;
            }
        }
        if (e_max < d->cfg.swept_cdb) {
            return -1;
        }
        const int32_t edge = e_max - PEAK_WIDTH_CDB;
        unsigned l = k_max, r = k_max;
        while (l > 0 && avg[l - 1] - q8_cdb(d->base_q8[l - 1]) >= edge) {
            l--;
        }
        while (r + 1 < n && avg[r + 1] - q8_cdb(d->base_q8[r + 1]) >= edge) {
            r++;
        }
        if (r - l + 1 <= d->cfg.swept_max_bins) {
            return (int)k_max;
        }
        for (unsigned k = l; k <= r; k++) {
            d->mark[k] |= MARK_BLANK;
        }
    }
    return -1;
}

/* Bins move this little in STATIONARY_MS: the peak is not sweeping. */
#define STATIONARY_MS 1500u

static void swept_update(detect_t *d, int pos, uint32_t t)
{
    if (pos < 0) {
        if (++d->sw_misses > 1) {
            swept_reset(d); /* two sweeps without a peak */
        }
        return;
    }
    d->sw_misses = 0;
    if (d->sw_pos < 0) {
        d->sw_pos = pos;
        d->sw_t = t;
        return;
    }
    const int delta = pos - d->sw_pos;
    const unsigned dist = (unsigned)abs(delta);
    const uint32_t dt = t - d->sw_t;
    const int sign = delta > 0 ? 1 : -1;
    if (d->sw_dir && sign == -d->sw_dir && dist >= d->n_bins / 2) {
        d->sw_pos = pos; /* sawtooth wrap: keep the run */
        d->sw_t = t;
        return;
    }
    if (dist < SWEPT_MIN_STEP || !dt) {
        if (dt >= STATIONARY_MS) {
            swept_reset(d);
            d->sw_pos = pos;
            d->sw_t = t;
        }
        return; /* otherwise wait for a measurable move from the same reference */
    }
    /* Speed in bins/s against the run's mean speed: a sweep moves steadily. */
    const uint32_t v = dist * 1000u / dt;
    const uint32_t mean = d->sw_run_ms ? (uint32_t)((uint64_t)d->sw_cover * 1000u / d->sw_run_ms) : v;
    const bool same = (!d->sw_dir || sign == d->sw_dir) && 2 * v >= mean && v <= 2 * mean;
    if (same) {
        d->sw_steps++;
        d->sw_cover += dist;
        d->sw_run_ms += dt;
    } else {
        d->sw_steps = 1;
        d->sw_cover = dist;
        d->sw_run_ms = dt;
    }
    d->sw_dir = sign;
    d->sw_pos = pos;
    d->sw_t = t;
}

static detect_kind_t strongest(uint8_t bits)
{
    for (int k = DETECT_NONE + 1; k < DETECT_KIND_COUNT; k++) {
        if (bits & DETECT_EV(k)) {
            return (detect_kind_t)k;
        }
    }
    return DETECT_NONE;
}

static void ema(int32_t *base_q8, int16_t now, const detect_config_t *c)
{
    int32_t diff = (int32_t)now * 256 - *base_q8;
    const int32_t clip = (int32_t)c->ema_clip_cdb * 256;
    diff = diff > clip ? clip : (diff < -clip ? -clip : diff);
    *base_q8 += diff / (1 << c->ema_shift);
}

/* Cell floors of the mean (s = 0) or min (s = 1) spectrum against their
 * baselines. Returns the number of cells that rose >= wide_cdb. */
static unsigned cell_rise(detect_t *d, int s, const int16_t *cdb)
{
    unsigned n = 0;
    for (unsigned c = 0; c < d->n_cells; c++) {
        d->cell_floor[s][c] = cell_floor_of(d, cdb, c);
        d->cell_delta[s][c] = (int16_t)(d->cell_floor[s][c] - q8_cdb(d->cell_q8[s][c]));
        n += d->cell_delta[s][c] >= d->cfg.wide_cdb;
    }
    return n;
}

bool detect_sweep(detect_t *d, const detect_input_t *in, detect_event_t *ev)
{
    const detect_state_t from = d->st.state;
    const uint32_t t = in->t_ms;
    detect_metrics_t m = {0};
    d->st.sweeps++;

    if (d->learn_started && in->gain != d->gain) {
        detect_relearn(d); /* levels are not comparable across gains */
    }
    if (d->st.state == DETECT_LEARNING) {
        if (learn_step(d, in)) {
            d->st.state = DETECT_NORMAL;
            d->st.kind = DETECT_NONE;
            d->last_ev_ms = d->last_strong_ms = t;
        }
        goto done;
    }

    /* Window: the sweeps of the last hold_s seconds, this one included. */
    d->t_head = (d->t_head + 1) % DETECT_HIST;
    d->t_hist[d->t_head] = t;
    d->t_count += d->t_count < DETECT_HIST;
    const uint32_t hold_ms = d->cfg.hold_s * 1000u;
    unsigned n_win = 0;
    while (n_win < d->t_count && t - d->t_hist[(d->t_head + DETECT_HIST - n_win) % DETECT_HIST] < hold_ms) {
        n_win++;
    }
    const uint32_t mask = n_win >= 32 ? UINT32_MAX : (1u << n_win) - 1u;
    m.window_sweeps = (uint8_t)n_win;

    /* Cells: floor rise of the mean and min spectra, occupancy, duty. */
    const unsigned wide_cells = cell_rise(d, 0, in->avg_cdb), cont_cells = cell_rise(d, 1, in->min_cdb);
    unsigned occ = 0, best = 0, max_duty = 0;
    for (unsigned c = 0; c < d->n_cells; c++) {
        unsigned lo, hi;
        cell_range(d, c, &lo, &hi);
        d->cell_hist[c] = (d->cell_hist[c] << 1) | (d->cell_delta[0][c] >= d->cfg.wide_cdb);
        const unsigned duty = pct_of((unsigned)__builtin_popcount(d->cell_hist[c] & mask), n_win);
        max_duty = duty > max_duty ? duty : max_duty;
        const int16_t base = q8_cdb(d->cell_q8[0][c]);
        for (unsigned k = lo; k < hi; k++) {
            occ += in->avg_cdb[k] - base >= d->cfg.occ_cdb;
        }
        best = d->cell_delta[0][c] > d->cell_delta[0][best] ? c : best;
    }
    {
        unsigned lo, hi;
        cell_range(d, best, &lo, &hi);
        m.floor_delta_max_cdb = d->cell_delta[0][best];
        m.floor_delta_max_khz = bp_bin_khz(d->plan, (lo + hi) / 2);
        m.flatness_cdb = flatness(in->avg_cdb, lo, hi);
    }
    m.floor_delta_cdb = median_of(d->cell_delta[0], d->n_cells, d->scratch);
    m.cont_delta_cdb = median_of(d->cell_delta[1], d->n_cells, d->scratch);
    m.wide_pct = (uint8_t)pct_of(wide_cells, d->n_cells);
    m.cont_pct = (uint8_t)pct_of(cont_cells, d->n_cells);
    m.occupancy_pct = (uint8_t)pct_of(occ, d->n_bins);
    m.duty_pct = (uint8_t)max_duty;
    m.clipped_pct = (uint8_t)pct_of(in->clipped_snapshots, in->snapshots);

    uint8_t bits = 0;
    if (cont_cells * 100u >= d->cfg.wide_pct * d->n_cells) {
        bits |= DETECT_EV(DETECT_WIDEBAND);
    } else if (wide_cells * 100u >= d->cfg.wide_pct * d->n_cells ||
               (in->snapshots && in->clipped_snapshots * 100u >= d->cfg.sat_pct * in->snapshots)) {
        bits |= DETECT_EV(DETECT_PULSED);
    }

    /* Narrowband: per-bin persistence of the min spectrum. */
    for (unsigned k = 0; k < d->n_bins; k++) {
        const bool hit = in->min_cdb[k] - q8_cdb(d->base_q8[k]) >= d->cfg.cw_cdb;
        d->cw_hist[k] = (d->cw_hist[k] << 1) | hit;
        const unsigned hits = (unsigned)__builtin_popcount(d->cw_hist[k] & mask);
        d->mark[k] = n_win >= 2 && hits * 100u >= d->cfg.cw_pct * n_win ? MARK_PERSIST : 0;
    }
    if (find_narrowband(d, in->min_cdb, &m)) {
        bits |= DETECT_EV(DETECT_NARROWBAND);
    }

    /* Swept: track the strongest narrow, non-persistent peak. Bins are
     * captured in frequency order, so interpolate when the peak was seen. */
    const int pos = find_swept_peak(d, in->avg_cdb);
    const uint32_t t0 = in->t_start_ms && in->t_start_ms <= t ? in->t_start_ms : t;
    swept_update(d, pos, pos < 0 ? t : t0 + (uint32_t)((uint64_t)(t - t0) * (unsigned)pos / d->n_bins));
    if (d->sw_pos >= 0) {
        m.swept_khz = bin_centre_khz(d, (unsigned)d->sw_pos);
    }
    m.swept_khz_per_s = d->sw_run_ms ? d->sw_dir * (int32_t)((uint64_t)d->sw_cover * d->plan->res_hz / d->sw_run_ms)
                                     : 0;
    m.swept_steps = (uint8_t)(d->sw_steps > 255 ? 255 : d->sw_steps);
    const unsigned cover = pct_of(d->sw_cover, d->n_bins);
    m.swept_cover_pct = (uint8_t)(cover > 100 ? 100 : cover);
    if (d->sw_steps >= d->cfg.swept_steps && cover >= d->cfg.swept_cover_pct) {
        bits |= DETECT_EV(DETECT_SWEPT);
    }

    /* State machine. */
    const bool any = bits != 0, strong = (bits & DETECT_EV_STRONG) != 0, prev_any = d->ev_hist & 1u;
    d->ev_hist = (d->ev_hist << 1) | any;
    d->strong_hist = (d->strong_hist << 1) | strong;
    const unsigned strong_duty = pct_of((unsigned)__builtin_popcount(d->strong_hist & mask), n_win);
    m.evidence = bits;
    m.evidence_duty_pct = (uint8_t)strong_duty;
    if (any) {
        d->last_ev_ms = t;
    }
    if (strong) {
        d->last_strong_ms = t;
    }
    switch (d->st.state) {
    case DETECT_NORMAL:
        if (any && prev_any) {
            d->st.state = DETECT_SUSPECT;
            d->st.kind = strongest(bits);
            d->suspect_since_ms = t;
        }
        break;
    case DETECT_SUSPECT:
        if (any) {
            d->st.kind = strongest(bits);
        }
        if (t - d->suspect_since_ms >= hold_ms && strong_duty >= d->cfg.duty_pct) {
            d->st.state = DETECT_ALARM;
            d->st.kind = strongest(bits & DETECT_EV_STRONG);
            d->st.alarms++;
            d->st.last_alarm_ms = t;
        } else if (t - d->last_ev_ms >= hold_ms) {
            d->st.state = DETECT_NORMAL;
            d->st.kind = DETECT_NONE;
        }
        break;
    case DETECT_ALARM:
        if (strong) {
            d->st.kind = strongest(bits & DETECT_EV_STRONG);
        }
        if (t - d->last_strong_ms >= d->cfg.clear_s * 1000u) {
            d->st.state = DETECT_NORMAL;
            d->st.kind = DETECT_NONE;
        }
        break;
    default:
        break;
    }

    /* Baseline follows slow changes, only while all is quiet. */
    if (d->st.state == DETECT_NORMAL && !any) {
        for (unsigned k = 0; k < d->n_bins; k++) {
            ema(&d->base_q8[k], in->avg_cdb[k], &d->cfg);
        }
        for (int s = 0; s < 2; s++) {
            for (unsigned c = 0; c < d->n_cells; c++) {
                ema(&d->cell_q8[s][c], d->cell_floor[s][c], &d->cfg);
            }
        }
    }
    d->st.m = m;

done:
    if (d->st.state != from) {
        d->st.state_since_ms = t;
        if (ev) {
            *ev = (detect_event_t){.from = from, .to = d->st.state, .kind = d->st.kind, .t_ms = t, .m = d->st.m};
        }
        return true;
    }
    return false;
}

void detect_get_status(const detect_t *d, detect_status_t *out)
{
    *out = d->st;
}

void detect_get_baseline(const detect_t *d, int16_t *out_cdb)
{
    for (unsigned k = 0; k < d->n_bins; k++) {
        out_cdb[k] = q8_cdb(d->base_q8[k]);
    }
}

const char *detect_state_name(detect_state_t s)
{
    static const char *names[] = {"LEARNING", "NORMAL", "SUSPECT", "ALARM"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

const char *detect_kind_name(detect_kind_t k)
{
    static const char *names[] = {"none", "wideband", "swept", "narrowband", "pulsed wideband"};
    return (unsigned)k < sizeof(names) / sizeof(names[0]) ? names[k] : "?";
}
