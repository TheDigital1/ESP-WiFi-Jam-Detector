/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp-wifi-jammer-detector, M2 radio test. Receive only.
 *
 * - Brings up the board (display on, so LCD/SPI coexistence is exercised)
 *   and the vendored esp-sdr receiver; logs RAM headroom below the
 *   capture-SRAM guard.
 * - At 2437 and 5500 MHz: K snapshots at 40 MS/s, 1024-point FFT, prints
 *   floor_db (20th percentile of the central 75 %), peak bin and clipping.
 * - Gain sweep at both frequencies shows clipping detection, then picks the
 *   highest gain without clipping ("auto-range" prototype).
 * - Then repeats one line per frequency every 0.5 s. Long-press BOOT to
 *   rerun the gain sweep.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "board.h"
#include "dsp.h"
#include "espsdr_rx.h"

static const char *TAG = "app";

#define FFT_N            1024
#define SNAPSHOTS        4            /* K per tune (brief section 5) */
#define RATE             RX_RATE_40M
#define USABLE_LO        (FFT_N / 8)  /* central 75 % */
#define USABLE_HI        (FFT_N - FFT_N / 8)
#define DC_GUARD         3            /* skip DC +/- 2 bins */
#define FLOOR_PCT        20
#define GAIN_STEP        4            /* display sweep */
#define AUTORANGE_STEP   2
#define CLIP_MAX_PPM     100          /* auto-range: at most 1e-4 of samples clipped */
#define PROBE_SNAPSHOTS  48           /* per auto-range step, spread over ~0.5 s */
#define PROBE_SPACING_MS 8
#define LONG_PRESS_US    1000000
#define CAPTURE_GUARD    0x40820000u  /* upstream sram_guard.ld */

extern char _bss_end;

typedef struct {
    int16_t floor_cdb, peak_cdb;
    unsigned peak_bin, clipped, samples, gain_code;
    uint32_t capture_us, total_us;
} measure_t;

static dsp_fft_t s_fft;
static int16_t *s_iq;
static uint32_t *s_power, *s_max;
static uint64_t *s_sum;
static int16_t *s_cdb, *s_scratch;

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "[%s] bss_end=%p, headroom below capture guard %u bytes; heap internal free %u "
             "(largest %u, min ever %u), PSRAM free %u",
             when, &_bss_end, (unsigned)(CAPTURE_GUARD - (uintptr_t)&_bss_end),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static esp_err_t alloc_buffers(void)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM;
    int16_t *window = heap_caps_malloc(FFT_N * sizeof(int16_t), caps);
    int16_t *twiddle = heap_caps_malloc(FFT_N * sizeof(int16_t), caps);
    int16_t *work = heap_caps_malloc(2 * FFT_N * sizeof(int16_t), caps);
    s_iq = heap_caps_malloc(2 * FFT_N * sizeof(int16_t), caps);
    s_power = heap_caps_malloc(FFT_N * sizeof(uint32_t), caps);
    s_max = heap_caps_malloc(FFT_N * sizeof(uint32_t), caps);
    s_sum = heap_caps_malloc(FFT_N * sizeof(uint64_t), caps);
    s_cdb = heap_caps_malloc(FFT_N * sizeof(int16_t), caps);
    s_scratch = heap_caps_malloc(FFT_N * sizeof(int16_t), caps);
    ESP_RETURN_ON_FALSE(window && twiddle && work && s_iq && s_power && s_max && s_sum && s_cdb && s_scratch,
                        ESP_ERR_NO_MEM, TAG, "PSRAM buffers");
    ESP_RETURN_ON_FALSE(dsp_fft_init(&s_fft, FFT_N, window, twiddle, work) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "fft init");
    return ESP_OK;
}

/* K snapshots at the current tuning: average power and per-bin max. */
static esp_err_t measure(measure_t *m)
{
    int64_t t0 = esp_timer_get_time();
    memset(s_sum, 0, FFT_N * sizeof(uint64_t));
    memset(s_max, 0, FFT_N * sizeof(uint32_t));
    memset(m, 0, sizeof(*m));
    for (unsigned k = 0; k < SNAPSHOTS; k++) {
        rx_capture_info_t info;
        ESP_RETURN_ON_ERROR(rx_capture(FFT_N, RATE, s_iq, &info), TAG, "capture %u", k);
        dsp_power_spectrum(&s_fft, s_iq, s_power);
        for (unsigned j = 0; j < FFT_N; j++) {
            s_sum[j] += s_power[j];
            if (s_power[j] > s_max[j]) {
                s_max[j] = s_power[j];
            }
        }
        m->clipped += info.clipped;
        m->samples += FFT_N;
        m->capture_us += info.capture_us;
        m->gain_code = info.gain_code;
    }
    for (unsigned j = 0; j < FFT_N; j++) {
        s_power[j] = (uint32_t)(s_sum[j] / SNAPSHOTS);
    }
    dsp_to_cdb(s_power, s_cdb, 0, FFT_N);
    m->floor_cdb = dsp_percentile(s_cdb, USABLE_LO, USABLE_HI, FFT_N / 2, DC_GUARD, FLOOR_PCT, s_scratch);
    m->peak_bin = dsp_peak_bin(s_cdb, USABLE_LO, USABLE_HI, FFT_N / 2, DC_GUARD);
    m->peak_cdb = s_cdb[m->peak_bin];
    m->total_us = (uint32_t)(esp_timer_get_time() - t0);
    return ESP_OK;
}

static float bin_offset_mhz(unsigned bin)
{
    return ((int)bin - FFT_N / 2) * (rx_rate_hz(RATE) / 1e6f) / FFT_N;
}

static void print_measure(const char *mode, const measure_t *m)
{
    ESP_LOGI(TAG, "%4u MHz %-6s gain=%2u floor_db=%6.2f peak_bin=%4u (%+6.2f MHz) peak_db=%6.2f "
             "clip=%u/%u (%u ppm) capture=%" PRIu32 "us total=%" PRIu32 "us",
             rx_frequency(), mode, m->gain_code, m->floor_cdb / 100.0f, m->peak_bin, bin_offset_mhz(m->peak_bin),
             m->peak_cdb / 100.0f, m->clipped, m->samples, (unsigned)((uint64_t)m->clipped * 1000000u / m->samples),
             m->capture_us, m->total_us);
}

static esp_err_t tune_timed(unsigned mhz)
{
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(rx_tune(mhz), TAG, "tune %u", mhz);
    ESP_LOGI(TAG, "tune -> %u MHz took %" PRId64 " us", mhz, esp_timer_get_time() - t0);
    return ESP_OK;
}

/* Clipping over PROBE_SNAPSHOTS snapshots spread across ~0.5 s, so bursty
 * Wi-Fi traffic gets a fair chance to show up. Returns parts per million. */
static esp_err_t clip_probe(unsigned *ppm, unsigned *worst_pct)
{
    uint64_t clipped = 0, samples = 0;
    unsigned worst = 0;
    for (unsigned k = 0; k < PROBE_SNAPSHOTS; k++) {
        rx_capture_info_t info;
        ESP_RETURN_ON_ERROR(rx_capture(FFT_N, RATE, s_iq, &info), TAG, "probe capture");
        clipped += info.clipped;
        samples += FFT_N;
        if (info.clipped * 100u / FFT_N > worst) {
            worst = info.clipped * 100u / FFT_N;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_SPACING_MS));
    }
    *ppm = (unsigned)(clipped * 1000000u / samples);
    *worst_pct = worst;
    return ESP_OK;
}

/* Print floor/peak/clipping across the gain range (shows clipping detection). */
static esp_err_t gain_sweep(unsigned mhz)
{
    ESP_RETURN_ON_ERROR(tune_timed(mhz), TAG, "sweep tune");
    unsigned max = rx_gain_max();
    ESP_LOGI(TAG, "gain sweep at %u MHz, indices 0..%u step %d", mhz, max, GAIN_STEP);
    for (unsigned g = 0;; g += GAIN_STEP) {
        if (g > max) {
            g = max; /* always finish on the top index */
        }
        ESP_RETURN_ON_ERROR(rx_set_gain_manual(g), TAG, "gain %u", g);
        measure_t m;
        ESP_RETURN_ON_ERROR(measure(&m), TAG, "measure");
        print_measure("manual", &m);
        if (g == max) {
            break;
        }
    }
    return ESP_OK;
}

/* Auto-range prototype: step down from the top gain until the probe sees at
 * most CLIP_MAX_PPM clipped samples. */
static esp_err_t auto_range(unsigned mhz, unsigned *best)
{
    ESP_RETURN_ON_ERROR(rx_tune(mhz), TAG, "auto-range tune");
    int g = (int)rx_gain_max();
    for (;; g -= AUTORANGE_STEP) {
        if (g < 0) {
            g = 0;
        }
        ESP_RETURN_ON_ERROR(rx_set_gain_manual((unsigned)g), TAG, "gain %d", g);
        unsigned ppm, worst;
        ESP_RETURN_ON_ERROR(clip_probe(&ppm, &worst), TAG, "probe");
        ESP_LOGI(TAG, "auto-range %u MHz gain %2d: clipping %u ppm over %d snapshots (worst snapshot %u%%)", mhz,
                 g, ppm, PROBE_SNAPSHOTS, worst);
        if (ppm <= CLIP_MAX_PPM || g == 0) {
            break;
        }
    }
    *best = (unsigned)g;
    ESP_LOGI(TAG, "auto-range %u MHz: chose gain %u", mhz, *best);
    return ESP_OK;
}

static esp_err_t run_sweeps(unsigned gains[2])
{
    static const unsigned freqs[2] = {2437, 5500};
    for (int i = 0; i < 2; i++) {
        ESP_RETURN_ON_ERROR(gain_sweep(freqs[i]), TAG, "sweep");
        ESP_RETURN_ON_ERROR(auto_range(freqs[i], &gains[i]), TAG, "auto-range");
    }
    return ESP_OK;
}

static esp_err_t radio_test(void)
{
    static const unsigned freqs[2] = {2437, 5500};
    measure_t m;

    /* Hardware AGC first, as the stock firmware runs. */
    ESP_RETURN_ON_ERROR(rx_set_gain_auto(), TAG, "agc");
    for (int i = 0; i < 2; i++) {
        ESP_RETURN_ON_ERROR(tune_timed(freqs[i]), TAG, "tune");
        ESP_RETURN_ON_ERROR(measure(&m), TAG, "measure");
        print_measure("agc", &m);
    }
    unsigned gains[2];
    ESP_RETURN_ON_ERROR(run_sweeps(gains), TAG, "sweeps");
    log_memory("after sweeps");

    ESP_LOGI(TAG, "monitoring at the auto-ranged gains (%u, %u); hold BOOT 1 s to rerun the sweep", gains[0],
             gains[1]);
    bool held = false;
    int64_t down = 0;
    for (;;) {
        for (int i = 0; i < 2; i++) {
            ESP_RETURN_ON_ERROR(rx_tune(freqs[i]), TAG, "tune");
            ESP_RETURN_ON_ERROR(rx_set_gain_manual(gains[i]), TAG, "gain");
            ESP_RETURN_ON_ERROR(measure(&m), TAG, "measure");
            print_measure("fixed", &m);
        }
        for (int t = 0; t < 25; t++) {
            int64_t now = esp_timer_get_time();
            if (board_boot_pressed()) {
                if (!held) {
                    held = true;
                    down = now;
                } else if (now - down >= LONG_PRESS_US) {
                    held = false;
                    ESP_LOGI(TAG, "BOOT long press: rerunning gain sweep");
                    ESP_RETURN_ON_ERROR(run_sweeps(gains), TAG, "sweeps");
                }
            } else {
                held = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static esp_err_t screen_init(void)
{
    uint16_t *row = heap_caps_calloc(BOARD_LCD_W * 20, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(row, ESP_ERR_NO_MEM, TAG, "screen buffer");
    for (unsigned i = 0; i < BOARD_LCD_W * 20; i++) {
        row[i] = board_rgb565(0, 0, 40);
    }
    esp_err_t err = ESP_OK;
    for (int y = 0; y < BOARD_LCD_H && err == ESP_OK; y += 20) {
        err = board_lcd_draw(0, y, BOARD_LCD_W, y + 20, row);
    }
    heap_caps_free(row);
    ESP_RETURN_ON_ERROR(err, TAG, "screen fill");
    return board_backlight_set(40);
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp-wifi-jammer-detector M2 radio test (receive only)");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    log_memory("boot");

    err = board_init(BOARD_ORIENT_LANDSCAPE);
    if (err == ESP_OK) {
        err = screen_init();
    }
    if (err == ESP_OK) {
        err = alloc_buffers();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup failed: %s", esp_err_to_name(err));
        return;
    }

    int64_t t0 = esp_timer_get_time();
    err = rx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "rx_init took %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
    log_memory("after rx_init");

    err = radio_test();
    ESP_LOGE(TAG, "radio test stopped: %s", esp_err_to_name(err));
}
