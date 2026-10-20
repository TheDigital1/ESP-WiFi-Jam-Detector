/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "dsp.h"
#include "sweep.h"

static const char *TAG = "sweep";

#define FFT_N            1024
#define DC_GUARD         3     /* notch DC +/- 2 bins */
#define TASK_PRIO        10    /* below the LVGL task (11, short I/O-bound bursts) */
#define CAPTURE_PRIO     12    /* only while a capture holds the radio-quiet window */
#define TASK_STACK       4096
#define QUIET_TIMEOUT_MS 200
#define STATS_PERIOD_US  10000000
/* Auto-range: step down until clipping stays at or below CLIP_MAX_PPM over
 * the probe, then back off AUTORANGE_MARGIN more indices (bursts that the
 * probe missed in M2 still clipped at the "clean" gain). */
#define CLIP_MAX_PPM     100
#define AUTORANGE_STEP   2
#define AUTORANGE_MARGIN 2
#define PROBE_SNAPS_MIN  96    /* per band and gain step, spread over the slices */
#define PROBE_SPACING_MS 4

typedef struct {
    band_plan_t plan;
    int16_t *avg_cdb, *max_cdb, *hold_cdb; /* stitched, PSRAM */
    unsigned gain;
    uint32_t sweeps, last_sweep_ms, window_sweeps, sweeps_per_10s;
    unsigned clipped_snapshots, clipped_acc;
    bool valid;
} band_state_t;

static sweep_config_t s_cfg;
/* Where a tune's time goes, summed over the stats period. */
static struct {
    int64_t tune_us, capture_us, dsp_us, yield_us;
    unsigned slices;
} s_prof;
static int64_t s_yield_debt_us;

/* CPU budget: after each piece of work, sleep in proportion to it so UI,
 * detect and IDLE get yield_pct of the CPU. The capture itself spends ~2 ms
 * inside upstream adctrig() per snapshot, so the sweep never blocks on its
 * own. Yielding after every snapshot (not once per tune) keeps the gaps
 * short, so lower-priority tasks see a few ms of latency, not ~60 ms. */
static void budget_yield(int64_t work_start_us)
{
    int64_t busy = esp_timer_get_time() - work_start_us;
    s_yield_debt_us += busy * s_cfg.yield_pct / (100 - s_cfg.yield_pct);
    if (s_yield_debt_us >= 1000) {
        uint32_t ms = (uint32_t)(s_yield_debt_us / 1000);
        s_yield_debt_us -= (int64_t)ms * 1000;
        s_prof.yield_us += (int64_t)ms * 1000;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}
static band_state_t s_band[BAND_COUNT];
static SemaphoreHandle_t s_lock;
static volatile sweep_state_t s_state = SWEEP_IDLE;
static volatile bool s_autorange;
static sweep_segment_cb_t s_seg_cb;
static void *s_seg_ctx;

/* DSP working set, PSRAM; only the sweep task touches it. */
static dsp_fft_t s_fft;
static int16_t *s_iq;
static uint32_t *s_power, *s_avg, *s_max;
static uint64_t *s_sum;
static uint32_t *s_gmin; /* grid-bin min over the snapshots, one slice */
static int16_t *s_seg_avg, *s_seg_max, *s_seg_min;

static void *psram_alloc(size_t bytes)
{
    return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
}

/* One capture inside the radio-quiet window (no LCD DMA while sampling).
 * The capture runs above the UI task's priority so the UI cannot pre-empt it
 * while it holds the window; it lasts ~2 ms. */
static esp_err_t capture(rx_capture_info_t *info)
{
    ESP_RETURN_ON_ERROR(board_rf_quiet_begin(QUIET_TIMEOUT_MS), TAG, "radio-quiet token");
    vTaskPrioritySet(NULL, CAPTURE_PRIO);
    esp_err_t err = rx_capture(FFT_N, s_cfg.rate, s_iq, info);
    vTaskPrioritySet(NULL, TASK_PRIO);
    board_rf_quiet_end();
    return err;
}

static esp_err_t autorange_band(band_state_t *b)
{
    const band_plan_t *p = &b->plan;
    unsigned top = UINT32_MAX;
    for (unsigned s = 0; s < p->n_slices; s++) {
        ESP_RETURN_ON_ERROR(rx_tune(p->center_mhz[s]), TAG, "tune");
        unsigned m = rx_gain_max();
        top = m < top ? m : top; /* the calibrated maximum varies a little per frequency */
    }
    const unsigned per_slice = (PROBE_SNAPS_MIN + p->n_slices - 1) / p->n_slices;
    int g = (int)top;
    for (;; g -= AUTORANGE_STEP) {
        if (g < 0) {
            g = 0;
        }
        uint64_t clipped = 0, samples = 0;
        unsigned worst_slice = 0, worst_ppm = 0;
        for (unsigned s = 0; s < p->n_slices; s++) {
            ESP_RETURN_ON_ERROR(rx_tune(p->center_mhz[s]), TAG, "tune");
            ESP_RETURN_ON_ERROR(rx_set_gain_manual((unsigned)g), TAG, "gain");
            uint64_t sc = 0;
            for (unsigned k = 0; k < per_slice; k++) {
                rx_capture_info_t info;
                ESP_RETURN_ON_ERROR(capture(&info), TAG, "probe");
                sc += info.clipped;
                vTaskDelay(pdMS_TO_TICKS(PROBE_SPACING_MS));
            }
            unsigned ppm = (unsigned)(sc * 1000000u / ((uint64_t)per_slice * FFT_N));
            if (ppm > worst_ppm) {
                worst_ppm = ppm;
                worst_slice = s;
            }
            clipped += sc;
            samples += (uint64_t)per_slice * FFT_N;
        }
        unsigned ppm = (unsigned)(clipped * 1000000u / samples);
        ESP_LOGI(TAG, "auto-range %s gain %2d: %u ppm clipped (worst %u ppm at %u MHz)", p->name, g, ppm, worst_ppm,
                 p->center_mhz[worst_slice]);
        if (ppm <= CLIP_MAX_PPM || g == 0) {
            break;
        }
    }
    g -= AUTORANGE_MARGIN;
    b->gain = g < 0 ? 0 : (unsigned)g;
    ESP_LOGI(TAG, "auto-range %s: gain %u (top %u, margin %d)", p->name, b->gain, top, AUTORANGE_MARGIN);
    return ESP_OK;
}

static esp_err_t sweep_slice(band_state_t *b, unsigned s, bool last)
{
    const band_plan_t *p = &b->plan;
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(rx_tune(p->center_mhz[s]), TAG, "tune %u", p->center_mhz[s]);
    ESP_RETURN_ON_ERROR(rx_set_gain_manual(b->gain), TAG, "gain");
    s_prof.tune_us += esp_timer_get_time() - t0;
    budget_yield(t0);

    const unsigned first = p->slice_first_bin[s], bins = p->slice_bins[s];
    memset(s_sum, 0, FFT_N * sizeof(uint64_t));
    memset(s_max, 0, FFT_N * sizeof(uint32_t));
    unsigned clipped_snaps = 0;
    for (unsigned k = 0; k < s_cfg.snapshots; k++) {
        rx_capture_info_t info;
        int64_t c0 = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(capture(&info), TAG, "capture");
        s_prof.capture_us += esp_timer_get_time() - c0;
        clipped_snaps += info.clipped > 0;
        dsp_power_spectrum(&s_fft, s_iq, s_power);
        /* Notch each snapshot, so the per-snapshot min and max never see the DC spike. */
        dsp_dc_notch(s_power, FFT_N, DC_GUARD);
        dsp_reduce_min(s_power, p->slice_fft_first[s], p->decim, bins, s_gmin, k == 0);
        for (unsigned j = 0; j < FFT_N; j++) {
            s_sum[j] += s_power[j];
            if (s_power[j] > s_max[j]) {
                s_max[j] = s_power[j];
            }
        }
        s_prof.dsp_us += esp_timer_get_time() - c0;
        budget_yield(c0);
    }
    int64_t t2 = esp_timer_get_time();
    for (unsigned j = 0; j < FFT_N; j++) {
        s_avg[j] = (uint32_t)(s_sum[j] / s_cfg.snapshots);
    }
    dsp_reduce(s_avg, s_max, p->slice_fft_first[s], p->decim, bins, s_seg_avg, s_seg_max);
    dsp_to_cdb(s_gmin, s_seg_min, 0, bins);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(b->avg_cdb + first, s_seg_avg, bins * sizeof(int16_t));
    memcpy(b->max_cdb + first, s_seg_max, bins * sizeof(int16_t));
    for (unsigned k = 0; k < bins; k++) {
        if (s_seg_max[k] > b->hold_cdb[first + k]) {
            b->hold_cdb[first + k] = s_seg_max[k];
        }
    }
    b->clipped_acc += clipped_snaps;
    xSemaphoreGive(s_lock);

    s_prof.dsp_us += esp_timer_get_time() - t2;
    s_prof.slices++;

    if (s_seg_cb) {
        const sweep_segment_t seg = {
            .band = p->id,
            .slice = s,
            .center_mhz = p->center_mhz[s],
            .first_bin = first,
            .bins = bins,
            .avg_cdb = s_seg_avg,
            .max_cdb = s_seg_max,
            .min_cdb = s_seg_min,
            .gain = b->gain,
            .snapshots = s_cfg.snapshots,
            .clipped_snapshots = clipped_snaps,
            .t_us = esp_timer_get_time(),
            .last_in_sweep = last,
        };
        s_seg_cb(&seg, s_seg_ctx);
    }

    budget_yield(t2);
    return ESP_OK;
}

static esp_err_t sweep_band(band_state_t *b)
{
    int64_t t0 = esp_timer_get_time();
    for (unsigned s = 0; s < b->plan.n_slices; s++) {
        ESP_RETURN_ON_ERROR(sweep_slice(b, s, s + 1 == b->plan.n_slices), TAG, "slice %u", s);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    b->last_sweep_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    b->sweeps++;
    b->window_sweeps++;
    b->clipped_snapshots = b->clipped_acc;
    b->clipped_acc = 0;
    b->valid = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* Auto-range the bands set to SWEEP_GAIN_AUTO (all of them when forced). */
static esp_err_t autorange(bool all)
{
    s_state = SWEEP_AUTORANGE;
    bool ranged = false;
    unsigned gain[BAND_COUNT];
    for (int i = 0; i < BAND_COUNT; i++) {
        unsigned g = s_band[i].gain;
        if (all || s_cfg.gain[i] == SWEEP_GAIN_AUTO) {
            ESP_RETURN_ON_ERROR(autorange_band(&s_band[i]), TAG, "auto-range");
            g = s_band[i].gain;
            ranged = true;
        } else {
            g = s_cfg.gain[i];
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_band[i].gain = gain[i] = g;
        s_band[i].valid = false; /* the view restarts at the new gain */
        memset(s_band[i].hold_cdb, 0, s_band[i].plan.n_bins * sizeof(int16_t));
        xSemaphoreGive(s_lock);
    }
    s_state = SWEEP_RUNNING;
    if (ranged && s_cfg.on_autorange) {
        s_cfg.on_autorange(gain);
    }
    return ESP_OK;
}

static void sweep_task(void *arg)
{
    esp_err_t err = autorange(false);
    int64_t window_start = esp_timer_get_time();
    while (err == ESP_OK) {
        for (int i = 0; i < BAND_COUNT && err == ESP_OK; i++) {
            for (unsigned w = 0; w < s_cfg.weight[i] && err == ESP_OK && !s_autorange; w++) {
                err = sweep_band(&s_band[i]);
            }
        }
        if (s_autorange && err == ESP_OK) {
            s_autorange = false;
            err = autorange(true);
        }
        int64_t now = esp_timer_get_time();
        if (now - window_start >= STATS_PERIOD_US) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < BAND_COUNT; i++) {
                band_state_t *b = &s_band[i];
                b->sweeps_per_10s = b->window_sweeps;
                b->window_sweeps = 0;
            }
            xSemaphoreGive(s_lock);
            for (int i = 0; i < BAND_COUNT; i++) {
                const band_state_t *b = &s_band[i];
                ESP_LOGI(TAG, "%-7s %2u slices, gain %2u: last sweep %4" PRIu32 " ms, %.2f sweeps/s, %u clipped of %u "
                         "snapshots in last sweep",
                         b->plan.name, b->plan.n_slices, b->gain, b->last_sweep_ms,
                         b->sweeps_per_10s * 1e6f / (float)(now - window_start), b->clipped_snapshots,
                         b->plan.n_slices * s_cfg.snapshots);
            }
            if (s_prof.slices) {
                const float n = (float)s_prof.slices;
                ESP_LOGI(TAG, "per tune: tune+recal %.1f ms, %u captures %.1f ms, DSP+stitch %.1f ms, yield %.1f ms",
                         s_prof.tune_us / n / 1000, s_cfg.snapshots, s_prof.capture_us / n / 1000,
                         (s_prof.dsp_us - s_prof.capture_us) / n / 1000, s_prof.yield_us / n / 1000);
            }
            memset(&s_prof, 0, sizeof(s_prof));
            window_start = now;
        }
    }
    ESP_LOGE(TAG, "sweep stopped: %s", esp_err_to_name(err));
    s_state = SWEEP_FAILED;
    vTaskDelete(NULL);
}

esp_err_t sweep_start(const sweep_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg->snapshots >= 1 && rx_rate_hz(cfg->rate) && cfg->yield_pct <= 90, ESP_ERR_INVALID_ARG,
                        TAG, "bad config");
    for (int i = 0; i < BAND_COUNT; i++) {
        ESP_RETURN_ON_FALSE(cfg->weight[i] >= 1 && cfg->weight[i] <= 8, ESP_ERR_INVALID_ARG, TAG, "bad weight");
    }
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");

    const bp_config_t bp = {
        .rate_hz = rx_rate_hz(cfg->rate), .fft_n = FFT_N, .usable_hz = cfg->usable_hz, .decim = 4,
    };
    for (int i = 0; i < BAND_COUNT; i++) {
        band_state_t *b = &s_band[i];
        ESP_RETURN_ON_FALSE(bp_build(&b->plan, (band_id_t)i, &bp) == 0, ESP_ERR_INVALID_ARG, TAG,
                            "band plan %d does not fit", i);
        b->avg_cdb = psram_alloc(b->plan.n_bins * sizeof(int16_t));
        b->max_cdb = psram_alloc(b->plan.n_bins * sizeof(int16_t));
        b->hold_cdb = psram_alloc(b->plan.n_bins * sizeof(int16_t));
        ESP_RETURN_ON_FALSE(b->avg_cdb && b->max_cdb && b->hold_cdb, ESP_ERR_NO_MEM, TAG, "band buffers");
        ESP_LOGI(TAG, "%s plan: %u slices of %" PRIu32 " MHz (centres %u..%u MHz), %u bins of %" PRIu32 " Hz",
                 b->plan.name, b->plan.n_slices, cfg->usable_hz / 1000000, b->plan.center_mhz[0],
                 b->plan.center_mhz[b->plan.n_slices - 1], b->plan.n_bins, b->plan.res_hz);
    }

    int16_t *window = psram_alloc(FFT_N * sizeof(int16_t));
    int16_t *twiddle = psram_alloc(FFT_N * sizeof(int16_t));
    int16_t *work = psram_alloc(2 * FFT_N * sizeof(int16_t));
    uint16_t *order = psram_alloc(FFT_N * sizeof(uint16_t));
    s_iq = psram_alloc(2 * FFT_N * sizeof(int16_t));
    s_power = psram_alloc(FFT_N * sizeof(uint32_t));
    s_avg = psram_alloc(FFT_N * sizeof(uint32_t));
    s_max = psram_alloc(FFT_N * sizeof(uint32_t));
    s_sum = psram_alloc(FFT_N * sizeof(uint64_t));
    s_seg_avg = psram_alloc(BP_MAX_BINS * sizeof(int16_t));
    s_seg_max = psram_alloc(BP_MAX_BINS * sizeof(int16_t));
    s_seg_min = psram_alloc(BP_MAX_BINS * sizeof(int16_t));
    s_gmin = psram_alloc(BP_MAX_BINS * sizeof(uint32_t));
    ESP_RETURN_ON_FALSE(window && twiddle && work && order && s_iq && s_power && s_avg && s_max && s_sum &&
                            s_seg_avg && s_seg_max && s_seg_min && s_gmin,
                        ESP_ERR_NO_MEM, TAG, "DSP buffers");
    ESP_RETURN_ON_FALSE(dsp_fft_init(&s_fft, FFT_N, window, twiddle, work, order) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "fft init");

    ESP_RETURN_ON_ERROR(rx_set_bw(cfg->bw_mhz), TAG, "bandwidth");
    ESP_RETURN_ON_FALSE(xTaskCreate(sweep_task, "sweep", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "sweep task");
    return ESP_OK;
}

sweep_state_t sweep_state(void)
{
    return s_state;
}

const band_plan_t *sweep_plan(band_id_t band)
{
    return band < BAND_COUNT ? &s_band[band].plan : NULL;
}

esp_err_t sweep_get_view(band_id_t band, sweep_view_t *out)
{
    ESP_RETURN_ON_FALSE(band < BAND_COUNT && out, ESP_ERR_INVALID_ARG, TAG, "bad view args");
    const band_state_t *b = &s_band[band];
    size_t bytes = b->plan.n_bins * sizeof(int16_t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->n_bins = b->plan.n_bins;
    if (out->avg_cdb) {
        memcpy(out->avg_cdb, b->avg_cdb, bytes);
    }
    if (out->max_cdb) {
        memcpy(out->max_cdb, b->max_cdb, bytes);
    }
    if (out->hold_cdb) {
        memcpy(out->hold_cdb, b->hold_cdb, bytes);
    }
    out->sweeps = b->sweeps;
    out->last_sweep_ms = b->last_sweep_ms;
    out->sweeps_per_10s = b->sweeps_per_10s;
    out->gain = b->gain;
    out->clipped_snapshots = b->clipped_snapshots;
    out->snapshots_per_sweep = b->plan.n_slices * s_cfg.snapshots;
    out->valid = b->valid;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void sweep_reset_maxhold(band_id_t band)
{
    if (band >= BAND_COUNT) {
        return;
    }
    band_state_t *b = &s_band[band];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(b->hold_cdb, b->max_cdb, b->plan.n_bins * sizeof(int16_t));
    xSemaphoreGive(s_lock);
}

esp_err_t sweep_set_weights(const unsigned weight[BAND_COUNT])
{
    for (int i = 0; i < BAND_COUNT; i++) {
        ESP_RETURN_ON_FALSE(weight[i] >= 1 && weight[i] <= 8, ESP_ERR_INVALID_ARG, TAG, "bad weight");
    }
    for (int i = 0; i < BAND_COUNT; i++) {
        s_cfg.weight[i] = weight[i]; /* read once per round by the sweep task */
    }
    return ESP_OK;
}

void sweep_autorange(void)
{
    s_autorange = true;
}

void sweep_set_segment_cb(sweep_segment_cb_t cb, void *ctx)
{
    s_seg_ctx = ctx;
    s_seg_cb = cb;
}
