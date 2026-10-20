/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host tests for main/detect.c, driven by spectra from tools/sim_spectra.py.
 * Run: make -C test_host   (or: build/test_detect SIM_DIR [-v])
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bandplan.h"
#include "detect.h"

static int failures;
static int verbose;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* Must match tools/sim_spectra.py. */
#define T_LEARNED_MS 65000u
#define T_EVENT_MS   70000u
#define T_EVENT_END  130000u

typedef enum {
    EXP_QUIET,    /* never leaves NORMAL after learning */
    EXP_NO_ALARM, /* SUSPECT allowed, never ALARM */
    EXP_SUSPECT,  /* at least SUSPECT during the event */
    EXP_ALARM,    /* ALARM of the given kind during the event */
} expect_t;

typedef struct {
    expect_t e;
    detect_kind_t kind;
} band_expect_t;

typedef struct {
    const char *name;
    band_expect_t band[BAND_COUNT];
} scenario_t;

static const scenario_t s_scenarios[] = {
    {"clean", {{EXP_QUIET, DETECT_NONE}, {EXP_QUIET, DETECT_NONE}}},
    {"wifi_busy", {{EXP_NO_ALARM, DETECT_NONE}, {EXP_NO_ALARM, DETECT_NONE}}},
    {"wideband24", {{EXP_ALARM, DETECT_WIDEBAND}, {EXP_QUIET, DETECT_NONE}}},
    {"wideband5", {{EXP_QUIET, DETECT_NONE}, {EXP_ALARM, DETECT_WIDEBAND}}},
    {"cw", {{EXP_ALARM, DETECT_NARROWBAND}, {EXP_ALARM, DETECT_NARROWBAND}}},
    {"swept", {{EXP_ALARM, DETECT_SWEPT}, {EXP_ALARM, DETECT_SWEPT}}},
    {"microwave", {{EXP_SUSPECT, DETECT_NONE}, {EXP_QUIET, DETECT_NONE}}},
    {"drift", {{EXP_QUIET, DETECT_NONE}, {EXP_QUIET, DETECT_NONE}}},
};

static band_plan_t s_plan[BAND_COUNT];
static detect_t s_det[BAND_COUNT];
static void *s_mem[BAND_COUNT];

static void build_plans(void)
{
    /* As the firmware: 80 MS/s, 1024-point FFT, 48 MHz per tune, 4 FFT bins per grid bin. */
    const bp_config_t cfg = {.rate_hz = 80000000, .fft_n = 1024, .usable_hz = 48000000, .decim = 4};
    for (int b = 0; b < BAND_COUNT; b++) {
        if (bp_build(&s_plan[b], (band_id_t)b, &cfg) || !(s_mem[b] = malloc(detect_mem_bytes(&s_plan[b])))) {
            printf("setup failed\n");
            exit(1);
        }
    }
}

static void init_detectors(void)
{
    detect_config_t cfg;
    detect_default_config(&cfg);
    for (int b = 0; b < BAND_COUNT; b++) {
        CHECK(detect_init(&s_det[b], &s_plan[b], &cfg, s_mem[b], detect_mem_bytes(&s_plan[b])) == 0, "init");
    }
}

static void print_metrics(const detect_metrics_t *m)
{
    printf("floor %+.1f/%+.1f dB (max %+.1f at %.1f MHz, flat %.1f) wide %u/%u%% occ %u%% duty %u%% clip %u%% "
           "ev 0x%x (%u%% of %u) cw %.2f MHz x%u %+.1f dB, swept %.1f MHz %+.1f MHz/s %u steps %u%%",
           m->floor_delta_cdb / 100.0, m->cont_delta_cdb / 100.0, m->floor_delta_max_cdb / 100.0,
           m->floor_delta_max_khz / 1000.0, m->flatness_cdb / 100.0, m->wide_pct, m->cont_pct, m->occupancy_pct,
           m->duty_pct, m->clipped_pct, m->evidence, m->evidence_duty_pct, m->window_sweeps, m->cw_khz / 1000.0,
           m->cw_bins, m->cw_excess_cdb / 100.0, m->swept_khz / 1000.0, m->swept_khz_per_s / 1000.0,
           m->swept_steps, m->swept_cover_pct);
}

typedef struct {
    detect_state_t worst_after_learn, worst_in_event;
    detect_kind_t alarm_kind;
    uint32_t first_alarm_ms, first_suspect_ms, suspect_ms, alarm_ms, last_t;
    detect_state_t state_at_learned;
    detect_state_t prev;
    uint32_t prev_t;
} band_track_t;

static int run_scenario(const char *dir, const scenario_t *sc)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bin", dir, sc->name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s (run tools/sim_spectra.py)\n", path);
        return 1;
    }
    char magic[8];
    uint32_t n = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "JDSIM2\0\0", 8) || fread(&n, 4, 1, f) != 1) {
        printf("FAIL bad file %s\n", path);
        fclose(f);
        return 1;
    }
    init_detectors();
    band_track_t tr[BAND_COUNT] = {0};
    static int16_t avg[BP_MAX_BINS], mn[BP_MAX_BINS], mx[BP_MAX_BINS];
    for (uint32_t r = 0; r < n; r++) {
        uint8_t hdr[20];
        if (fread(hdr, 1, 20, f) != 20) {
            break;
        }
        const unsigned band = hdr[0], nb = hdr[2] | hdr[3] << 8;
        uint32_t t0, t;
        uint16_t gain, clipped, snaps;
        memcpy(&t0, hdr + 4, 4);
        memcpy(&t, hdr + 8, 4);
        memcpy(&gain, hdr + 12, 2);
        memcpy(&clipped, hdr + 14, 2);
        memcpy(&snaps, hdr + 16, 2);
        if (band >= BAND_COUNT || nb != s_plan[band].n_bins || fread(avg, 2, nb, f) != nb ||
            fread(mn, 2, nb, f) != nb || fread(mx, 2, nb, f) != nb) {
            printf("FAIL record %u of %s: band %u, %u bins\n", (unsigned)r, sc->name, band, nb);
            fclose(f);
            return 1;
        }
        const detect_input_t in = {
            .avg_cdb = avg, .min_cdb = mn, .max_cdb = mx, .clipped_snapshots = clipped, .snapshots = snaps,
            .gain = gain, .t_start_ms = t0, .t_ms = t,
        };
        detect_event_t ev;
        band_track_t *b = &tr[band];
        if (detect_sweep(&s_det[band], &in, &ev)) {
            if (verbose) {
                printf("  %6.1f s %-7s %s -> %s (%s): ", t / 1000.0, s_plan[band].name, detect_state_name(ev.from),
                       detect_state_name(ev.to), detect_kind_name(ev.kind));
                print_metrics(&ev.m);
                printf("\n");
            }
            if (ev.to == DETECT_ALARM && !b->first_alarm_ms) {
                b->first_alarm_ms = t;
                b->alarm_kind = ev.kind;
            }
            if (ev.to == DETECT_SUSPECT && !b->first_suspect_ms) {
                b->first_suspect_ms = t;
            }
        }
        detect_status_t st;
        detect_get_status(&s_det[band], &st);
        if (b->prev_t) {
            if (b->prev == DETECT_SUSPECT) {
                b->suspect_ms += t - b->prev_t;
            } else if (b->prev == DETECT_ALARM) {
                b->alarm_ms += t - b->prev_t;
            }
        }
        b->prev = st.state;
        b->prev_t = t;
        if (t < T_LEARNED_MS) {
            b->state_at_learned = st.state;
        } else if (st.state > b->worst_after_learn) {
            b->worst_after_learn = st.state;
        }
        if (t >= T_EVENT_MS && t < T_EVENT_END + 5000u && st.state > b->worst_in_event) {
            b->worst_in_event = st.state;
        }
        b->last_t = t;
        if (verbose > 1) {
            printf("  %6.1f s %-7s %-8s ", t / 1000.0, s_plan[band].name, detect_state_name(st.state));
            print_metrics(&st.m);
            printf("\n");
        }
    }
    fclose(f);

    int before = failures;
    for (int band = 0; band < BAND_COUNT; band++) {
        const band_track_t *b = &tr[band];
        const band_expect_t *x = &sc->band[band];
        detect_status_t st;
        detect_get_status(&s_det[band], &st);
        printf("  %-10s %-7s worst %-8s suspect %5.1f s alarm %5.1f s", sc->name, s_plan[band].name,
               detect_state_name(b->worst_after_learn), b->suspect_ms / 1000.0, b->alarm_ms / 1000.0);
        if (b->first_alarm_ms) {
            printf(", ALARM (%s) %.1f s after onset", detect_kind_name(b->alarm_kind),
                   ((int32_t)b->first_alarm_ms - (int32_t)T_EVENT_MS) / 1000.0);
        }
        printf("\n");
        CHECK(b->state_at_learned == DETECT_NORMAL || b->state_at_learned == DETECT_LEARNING, "%s %s: learn",
              sc->name, s_plan[band].name);
        CHECK(b->worst_after_learn != DETECT_LEARNING, "%s %s: baseline not learned after %u ms", sc->name,
              s_plan[band].name, T_LEARNED_MS);
        switch (x->e) {
        case EXP_QUIET:
            CHECK(b->worst_after_learn == DETECT_NORMAL, "%s %s: expected NORMAL throughout, got %s", sc->name,
                  s_plan[band].name, detect_state_name(b->worst_after_learn));
            break;
        case EXP_NO_ALARM:
            CHECK(b->worst_after_learn < DETECT_ALARM, "%s %s: false ALARM", sc->name, s_plan[band].name);
            break;
        case EXP_SUSPECT:
            CHECK(b->worst_in_event >= DETECT_SUSPECT, "%s %s: expected at least SUSPECT, got %s", sc->name,
                  s_plan[band].name, detect_state_name(b->worst_in_event));
            break;
        case EXP_ALARM:
            CHECK(b->first_alarm_ms >= T_EVENT_MS && b->first_alarm_ms < T_EVENT_END, "%s %s: expected ALARM",
                  sc->name, s_plan[band].name);
            CHECK(b->alarm_kind == x->kind, "%s %s: ALARM kind %s, expected %s", sc->name, s_plan[band].name,
                  detect_kind_name(b->alarm_kind), detect_kind_name(x->kind));
            CHECK(b->first_suspect_ms >= T_EVENT_MS, "%s %s: SUSPECT before the event", sc->name,
                  s_plan[band].name);
            break;
        }
        CHECK(st.state == DETECT_NORMAL, "%s %s: ends in %s, expected NORMAL", sc->name, s_plan[band].name,
              detect_state_name(st.state));
    }
    return failures != before;
}

static void test_config_and_api(void)
{
    detect_config_t cfg;
    detect_default_config(&cfg);
    CHECK(detect_check_config(&cfg) == 0, "defaults valid");
    detect_config_t bad = cfg;
    bad.wide_pct = 0;
    CHECK(detect_check_config(&bad) != 0, "wide_pct 0 rejected");
    bad = cfg;
    bad.hold_s = 0;
    CHECK(detect_check_config(&bad) != 0, "hold_s 0 rejected");

    detect_t d;
    const band_plan_t *p = &s_plan[BAND_24];
    CHECK(detect_init(&d, p, &cfg, s_mem[0], detect_mem_bytes(p) - 1) != 0, "short memory rejected");
    CHECK(detect_init(&d, p, &bad, s_mem[0], detect_mem_bytes(p)) != 0, "bad config rejected");
    cfg.learn_s = 5;
    CHECK(detect_init(&d, p, &cfg, s_mem[0], detect_mem_bytes(p)) == 0, "init");

    /* Flat noise-free spectrum: learns in learn_s, then stays NORMAL. */
    static int16_t flat[BP_MAX_BINS], tone[BP_MAX_BINS];
    for (unsigned k = 0; k < p->n_bins; k++) {
        flat[k] = 3000;
    }
    detect_input_t in = {.avg_cdb = flat, .min_cdb = flat, .max_cdb = flat, .snapshots = 8, .gain = 60};
    detect_event_t ev;
    detect_status_t st;
    bool learned = false;
    for (uint32_t t = 0; t <= 6000; t += 500) {
        in.t_ms = t;
        if (detect_sweep(&d, &in, &ev)) {
            learned = ev.from == DETECT_LEARNING && ev.to == DETECT_NORMAL;
            CHECK(t >= 5000, "learned too early (%u ms)", (unsigned)t);
        }
    }
    CHECK(learned, "learning finished");
    int16_t base[BP_MAX_BINS];
    detect_get_baseline(&d, base);
    CHECK(base[0] == 3000 && base[p->n_bins - 1] == 3000, "baseline %d %d", base[0], base[p->n_bins - 1]);

    /* Baseline record: round trip into a fresh detector; wrong plan rejected. */
    {
        static uint8_t rec[64 * 1024];
        const size_t n = detect_save_baseline(&d, rec, sizeof(rec));
        CHECK(n == detect_baseline_bytes(p) && n > 0, "baseline record %zu bytes", n);
        CHECK(detect_save_baseline(&d, rec, n - 1) == 0, "short record buffer rejected");
        detect_t d2;
        CHECK(detect_init(&d2, p, &cfg, s_mem[1], detect_mem_bytes(p)) == 0, "init 2");
        CHECK(detect_save_baseline(&d2, rec + n, sizeof(rec) - n) == 0, "no record while learning");
        CHECK(detect_load_baseline(&d2, rec, n - 1) != 0, "truncated record rejected");
        CHECK(detect_load_baseline(&d2, rec, n) == 0, "record loads");
        int16_t base2[BP_MAX_BINS];
        detect_get_baseline(&d2, base2);
        detect_get_status(&d2, &st);
        CHECK(st.state == DETECT_NORMAL && !memcmp(base, base2, p->n_bins * sizeof(int16_t)), "restored baseline");
        detect_t d5;
        const band_plan_t *p5 = &s_plan[BAND_5];
        CHECK(detect_init(&d5, p5, &cfg, s_mem[1], detect_mem_bytes(p5)) == 0, "init 5");
        CHECK(detect_load_baseline(&d5, rec, n) != 0, "2.4 GHz record rejected by the 5 GHz plan");
        detect_get_status(&d5, &st);
        CHECK(st.state == DETECT_LEARNING, "5 GHz still learning");
        /* Restored at gain 60; a sweep at another gain re-learns. */
        CHECK(detect_init(&d2, p, &cfg, s_mem[1], detect_mem_bytes(p)) == 0 && detect_load_baseline(&d2, rec, n) == 0,
              "reload");
        detect_input_t g = in;
        g.gain = 50;
        g.t_ms = 6000;
        detect_sweep(&d2, &g, &ev);
        detect_get_status(&d2, &st);
        CHECK(st.state == DETECT_LEARNING, "restored baseline at another gain -> %s", detect_state_name(st.state));
    }

    /* A steady +20 dB tone: SUSPECT, then ALARM after hold_s, NORMAL clear_s after it stops. */
    memcpy(tone, flat, sizeof(tone));
    tone[100] = 5000;
    in.avg_cdb = in.min_cdb = in.max_cdb = tone;
    uint32_t t = 6500, t_alarm = 0, t_normal = 0;
    for (; t < 20000; t += 500) {
        in.t_ms = t;
        if (detect_sweep(&d, &in, &ev) && ev.to == DETECT_ALARM) {
            t_alarm = t;
            CHECK(ev.kind == DETECT_NARROWBAND, "tone kind %s", detect_kind_name(ev.kind));
            CHECK(ev.m.cw_bins == 1 && ev.m.cw_excess_cdb > 1900 && ev.m.cw_excess_cdb <= 2000, "tone metrics %u %d", ev.m.cw_bins,
                  ev.m.cw_excess_cdb);
        }
    }
    /* Persistent for hold_s (window) before it is evidence, then SUSPECT for hold_s. */
    CHECK(t_alarm >= 6500 + 9000 && t_alarm <= 6500 + 11000, "tone ALARM at %u ms", (unsigned)t_alarm);
    in.avg_cdb = in.min_cdb = in.max_cdb = flat;
    for (uint32_t t_off = t; t < t_off + 15000; t += 500) {
        in.t_ms = t;
        if (detect_sweep(&d, &in, &ev) && ev.to == DETECT_NORMAL && !t_normal) {
            t_normal = t - t_off;
        }
    }
    CHECK(t_normal >= 10000 && t_normal <= 11000, "ALARM cleared after %u ms", (unsigned)t_normal);

    /* A gain change restarts learning. */
    in.gain = 61;
    in.t_ms = t;
    detect_sweep(&d, &in, &ev);
    detect_get_status(&d, &st);
    CHECK(st.state == DETECT_LEARNING, "gain change -> %s", detect_state_name(st.state));
    CHECK(st.alarms == 1, "alarm count %u", (unsigned)st.alarms);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/sim";
    const char *only = NULL; /* optional scenario name */
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') {
            verbose += !strcmp(argv[i], "-v") + 2 * !strcmp(argv[i], "-vv");
        } else {
            only = argv[i];
        }
    }
    build_plans();
    test_config_and_api();
    for (size_t i = 0; i < sizeof(s_scenarios) / sizeof(s_scenarios[0]); i++) {
        if (!only || !strcmp(only, s_scenarios[i].name)) {
            run_scenario(dir, &s_scenarios[i]);
        }
    }
    printf(failures ? "test_detect: %d FAILED\n" : "test_detect: all passed\n", failures);
    return failures ? 1 : 0;
}
