/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "detector.h"
#include "eventlog.h"
#include "settings.h"
#include "sweep.h"

static const char *TAG = "detect";

#define TASK_PRIO       9 /* below the sweep (10): never delays a capture */
#define TASK_STACK      4096
#define STATUS_LOG_MS   10000
#define SPECTRA         3 /* avg, min, max */
#define SAVE_PERIOD_MS  (60u * 60u * 1000u) /* keep the stored baseline close to the EMA-tracked one */

typedef struct {
    unsigned clipped, snapshots, gain, slices;
    uint32_t t_start_ms, t_ms;
} sweep_meta_t;

/* Two whole-band buffers per band: the sweep fills one while the task reads
 * the other. If the task is still busy when a sweep completes, that sweep
 * is dropped (counted) rather than blocking the sweep. */
typedef struct {
    const band_plan_t *plan;
    detect_t det;
    int16_t *spec[2][SPECTRA];
    sweep_meta_t meta[2];
    volatile bool busy[2];
    unsigned fill;
    uint32_t dropped, incomplete;
    uint32_t saved_ms; /* last baseline save */
    uint32_t logged_lost; /* dropped + incomplete at the last warning */
} band_ctx_t;

static band_ctx_t s_band[BAND_COUNT];
static QueueHandle_t s_queue; /* (band << 1) | slot */
static SemaphoreHandle_t s_lock;
static volatile bool s_relearn;
static uint8_t *s_rec; /* baseline record, PSRAM; detect task only (and start) */
static size_t s_rec_cap;

/* Sweep task context: copy and hand over, nothing else. */
static void on_segment(const sweep_segment_t *seg, void *ctx)
{
    (void)ctx;
    band_ctx_t *b = &s_band[seg->band];
    const unsigned slot = b->fill;
    sweep_meta_t *m = &b->meta[slot];
    if (seg->slice == 0) {
        *m = (sweep_meta_t){.t_start_ms = (uint32_t)(seg->t_us / 1000), .gain = seg->gain};
    }
    const size_t bytes = seg->bins * sizeof(int16_t);
    memcpy(b->spec[slot][0] + seg->first_bin, seg->avg_cdb, bytes);
    memcpy(b->spec[slot][1] + seg->first_bin, seg->min_cdb, bytes);
    memcpy(b->spec[slot][2] + seg->first_bin, seg->max_cdb, bytes);
    m->clipped += seg->clipped_snapshots;
    m->snapshots += seg->snapshots;
    m->slices++;
    m->gain = seg->gain;
    if (!seg->last_in_sweep) {
        return;
    }
    m->t_ms = (uint32_t)(seg->t_us / 1000);
    if (m->slices != b->plan->n_slices) {
        b->incomplete++; /* started mid-sweep */
        return;
    }
    if (b->busy[slot ^ 1]) {
        b->dropped++; /* refill this slot */
        return;
    }
    b->busy[slot] = true;
    const uint8_t item = (uint8_t)(seg->band << 1 | slot);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        b->busy[slot] = false;
        b->dropped++;
        return;
    }
    b->fill = slot ^ 1;
}

static void log_metrics(esp_log_level_t level, const char *band, const char *what, const detect_metrics_t *m)
{
    ESP_LOG_LEVEL(level, TAG,
                  "%s %s: floor %+.1f dB, continuous %+.1f dB, max cell %+.1f dB at %.0f MHz (flatness %.1f dB), "
                  "wide %u%%/%u%% of cells, occupancy %u%%, duty %u%%, clipped %u%%, evidence 0x%x (%u%% of %u "
                  "sweeps), cw %.2f MHz x%u %+.1f dB, swept %.1f MHz %+.1f MHz/s %u steps %u%%",
                  band, what, m->floor_delta_cdb / 100.0f, m->cont_delta_cdb / 100.0f,
                  m->floor_delta_max_cdb / 100.0f, m->floor_delta_max_khz / 1000.0f, m->flatness_cdb / 100.0f,
                  m->wide_pct, m->cont_pct, m->occupancy_pct, m->duty_pct, m->clipped_pct, m->evidence,
                  m->evidence_duty_pct, m->window_sweeps, m->cw_khz / 1000.0f, m->cw_bins,
                  m->cw_excess_cdb / 100.0f, m->swept_khz / 1000.0f, m->swept_khz_per_s / 1000.0f, m->swept_steps,
                  m->swept_cover_pct);
}

/* Detect task: the baseline only changes here, so no lock is needed to read it. */
static void save_baseline(band_ctx_t *b, uint32_t t_ms)
{
    const size_t n = detect_save_baseline(&b->det, s_rec, s_rec_cap);
    if (!n) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = settings_save_baseline(b->plan->id, s_rec, n);
    b->saved_ms = t_ms;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s baseline saved (%u bytes, %u ms)", b->plan->name, (unsigned)n,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGW(TAG, "%s baseline save: %s", b->plan->name, esp_err_to_name(err));
    }
}

static void detector_task(void *arg)
{
    (void)arg;
    uint32_t last_log[BAND_COUNT] = {0};
    for (;;) {
        uint8_t item;
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_relearn) {
            s_relearn = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < BAND_COUNT; i++) {
                detect_relearn(&s_band[i].det);
            }
            xSemaphoreGive(s_lock);
            const esp_err_t err = settings_erase_baselines();
            ESP_LOGI(TAG, "re-learning baselines (stored ones erased: %s)", esp_err_to_name(err));
        }
        band_ctx_t *b = &s_band[item >> 1];
        const unsigned slot = item & 1u;
        const sweep_meta_t *m = &b->meta[slot];
        const detect_input_t in = {
            .avg_cdb = b->spec[slot][0],
            .min_cdb = b->spec[slot][1],
            .max_cdb = b->spec[slot][2],
            .clipped_snapshots = m->clipped,
            .snapshots = m->snapshots,
            .gain = m->gain,
            .t_start_ms = m->t_start_ms,
            .t_ms = m->t_ms,
        };
        detect_event_t ev;
        detect_status_t st;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool changed = detect_sweep(&b->det, &in, &ev);
        detect_get_status(&b->det, &st);
        xSemaphoreGive(s_lock);
        b->busy[slot] = false;

        const char *name = b->plan->name;
        if (changed) {
            eventlog_post(b->plan->id, &ev);
            if (ev.from == DETECT_LEARNING && ev.to == DETECT_NORMAL) {
                ESP_LOGI(TAG, "%s baseline learned (gain %u)", name, m->gain);
                save_baseline(b, m->t_ms);
            } else if (ev.to == DETECT_LEARNING) {
                ESP_LOGI(TAG, "%s gain changed to %u: re-learning baseline", name, m->gain);
            } else {
                char what[64];
                snprintf(what, sizeof(what), "%s -> %s (%s)", detect_state_name(ev.from), detect_state_name(ev.to),
                         detect_kind_name(ev.kind));
                log_metrics(ev.to >= DETECT_SUSPECT ? ESP_LOG_WARN : ESP_LOG_INFO, name, what, &ev.m);
            }
        }
        if (st.state == DETECT_NORMAL && m->t_ms - b->saved_ms >= SAVE_PERIOD_MS) {
            save_baseline(b, m->t_ms);
        }
        if (m->t_ms - last_log[item >> 1] >= STATUS_LOG_MS) {
            last_log[item >> 1] = m->t_ms;
            if (st.state == DETECT_LEARNING) {
                ESP_LOGI(TAG, "%s learning baseline %u%%", name, st.learn_pct);
            } else {
                log_metrics(ESP_LOG_INFO, name, detect_state_name(st.state), &st.m);
            }
            if (b->dropped + b->incomplete != b->logged_lost) {
                b->logged_lost = b->dropped + b->incomplete;
                ESP_LOGW(TAG, "%s: %" PRIu32 " sweeps dropped (detector busy), %" PRIu32 " incomplete", name,
                         b->dropped, b->incomplete);
            }
        }
    }
}

esp_err_t detector_start(const detect_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(detect_check_config(cfg) == 0, ESP_ERR_INVALID_ARG, TAG, "bad config");
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(uint8_t));
    ESP_RETURN_ON_FALSE(s_lock && s_queue, ESP_ERR_NO_MEM, TAG, "sync");
    for (int i = 0; i < BAND_COUNT; i++) {
        band_ctx_t *b = &s_band[i];
        b->plan = sweep_plan((band_id_t)i);
        ESP_RETURN_ON_FALSE(b->plan && b->plan->n_bins, ESP_ERR_INVALID_STATE, TAG, "sweep not started");
        const size_t bytes = detect_mem_bytes(b->plan);
        void *mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(mem, ESP_ERR_NO_MEM, TAG, "detector memory");
        ESP_RETURN_ON_FALSE(detect_init(&b->det, b->plan, cfg, mem, bytes) == 0, ESP_ERR_INVALID_ARG, TAG,
                            "detect init");
        const size_t rec = detect_baseline_bytes(b->plan);
        s_rec_cap = rec > s_rec_cap ? rec : s_rec_cap;
        for (int s = 0; s < 2; s++) {
            for (int k = 0; k < SPECTRA; k++) {
                b->spec[s][k] = heap_caps_calloc(b->plan->n_bins, sizeof(int16_t), MALLOC_CAP_SPIRAM);
                ESP_RETURN_ON_FALSE(b->spec[s][k], ESP_ERR_NO_MEM, TAG, "spectra");
            }
        }
        ESP_LOGI(TAG, "%s: %u cells of %u bins, %u KB working memory", b->plan->name, b->det.n_cells,
                 DETECT_CELL_BINS, (unsigned)(bytes / 1024));
    }
    s_rec = heap_caps_malloc(s_rec_cap, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rec, ESP_ERR_NO_MEM, TAG, "baseline record");
    for (int i = 0; i < BAND_COUNT; i++) {
        band_ctx_t *b = &s_band[i];
        size_t len = 0;
        esp_err_t err = settings_load_baseline((band_id_t)i, s_rec, s_rec_cap, &len);
        if (err == ESP_OK && detect_load_baseline(&b->det, s_rec, len) == 0) {
            ESP_LOGI(TAG, "%s: stored baseline restored (re-learns if the gain differs)", b->plan->name);
        } else {
            ESP_LOGI(TAG, "%s: no usable stored baseline (%s), learning", b->plan->name,
                     err == ESP_OK ? "mismatch" : esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "learn %u s; wideband %.1f dB in %u%% of cells; narrowband %.1f dB in %u%% of sweeps; "
             "swept %.1f dB over %u%% of band; alarm after %u s at %u%% duty, clear after %u s",
             cfg->learn_s, cfg->wide_cdb / 100.0f, cfg->wide_pct, cfg->cw_cdb / 100.0f, cfg->cw_pct,
             cfg->swept_cdb / 100.0f, cfg->swept_cover_pct, cfg->hold_s, cfg->duty_pct, cfg->clear_s);
    ESP_RETURN_ON_FALSE(xTaskCreate(detector_task, "detect", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    sweep_set_segment_cb(on_segment, NULL);
    return ESP_OK;
}

esp_err_t detector_get_status(band_id_t band, detect_status_t *out)
{
    ESP_RETURN_ON_FALSE(band < BAND_COUNT && out && s_lock, ESP_ERR_INVALID_ARG, TAG, "bad args");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    detect_get_status(&s_band[band].det, out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t detector_get_baseline(band_id_t band, int16_t *out)
{
    ESP_RETURN_ON_FALSE(band < BAND_COUNT && out && s_lock, ESP_ERR_INVALID_ARG, TAG, "bad args");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    detect_get_baseline(&s_band[band].det, out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t detector_set_config(const detect_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && detect_check_config(cfg) == 0 && s_lock, ESP_ERR_INVALID_ARG, TAG, "bad config");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BAND_COUNT; i++) {
        detect_set_config(&s_band[i].det, cfg);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void detector_relearn(void)
{
    s_relearn = true;
}
