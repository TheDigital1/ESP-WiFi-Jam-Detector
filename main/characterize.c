/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Optional boot-time receiver characterisation (CONFIG_JD_CHARACTERIZE):
 * 1. Filter shape per sample rate / analog bandwidth: median noise spectrum
 *    over 8 tunes in a quiet 5 GHz stretch, in 32 groups relative to centre.
 * 2. LCD self-interference: spectra with the LCD idle vs continuously
 *    redrawing, alternated over 4 cycles, first with captures free to overlap
 *    LCD DMA, then inside the radio-quiet window the sweep uses.
 * Results only go to the log. Runs before LVGL starts.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "characterize.h"
#include "dsp.h"
#include "espsdr_rx.h"

static const char *TAG = "characterize";

#define FFT_N       1024
#define TUNES       8
#define SNAPS       32
#define GROUPS      32
#define GAIN        70
#define SHAPE_START 5490 /* quiet DFS stretch; 2.4 GHz Wi-Fi swamps the median */
#define LCD_CYCLES  4
#define LCD_SNAPS   64
#define DC_GUARD    3
#define HAMMER_PRIO 3

static dsp_fft_t s_fft;
static int16_t *s_iq, *s_cdb, *s_scratch, *s_rows;
static uint32_t *s_power;
static uint64_t *s_sum;
static volatile bool s_lcd_busy;
static bool s_quiet;

static void lcd_hammer_task(void *arg)
{
    uint16_t *buf = arg;
    for (;;) {
        if (!s_lcd_busy) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int y = 0; y < BOARD_LCD_H && s_lcd_busy; y += 20) {
            if (board_lcd_draw(0, y, BOARD_LCD_W, y + 20, buf) != ESP_OK) {
                break;
            }
        }
    }
}

static esp_err_t capture(rx_rate_t rate, rx_capture_info_t *info)
{
    if (!s_quiet) {
        return rx_capture(FFT_N, rate, s_iq, info);
    }
    ESP_RETURN_ON_ERROR(board_rf_quiet_begin(200), TAG, "quiet");
    esp_err_t err = rx_capture(FFT_N, rate, s_iq, info);
    board_rf_quiet_end();
    return err;
}

static esp_err_t mean_cdb(rx_rate_t rate, unsigned snaps, int16_t *out)
{
    memset(s_sum, 0, FFT_N * sizeof(uint64_t));
    for (unsigned k = 0; k < snaps; k++) {
        rx_capture_info_t info;
        ESP_RETURN_ON_ERROR(capture(rate, &info), TAG, "capture");
        dsp_power_spectrum(&s_fft, s_iq, s_power);
        for (unsigned j = 0; j < FFT_N; j++) {
            s_sum[j] += s_power[j];
        }
    }
    for (unsigned j = 0; j < FFT_N; j++) {
        s_power[j] = (uint32_t)(s_sum[j] / snaps);
    }
    dsp_to_cdb(s_power, out, 0, FFT_N);
    return ESP_OK;
}

static int cmp_i16(const void *a, const void *b)
{
    return *(const int16_t *)a - *(const int16_t *)b;
}

static esp_err_t filter_shape(rx_rate_t rate, unsigned bw)
{
    ESP_RETURN_ON_ERROR(rx_set_bw(bw), TAG, "bw");
    int64_t t0 = esp_timer_get_time();
    for (int t = 0; t < TUNES; t++) {
        ESP_RETURN_ON_ERROR(rx_tune(SHAPE_START + 10 * t), TAG, "tune");
        ESP_RETURN_ON_ERROR(rx_set_gain_manual(GAIN), TAG, "gain");
        ESP_RETURN_ON_ERROR(mean_cdb(rate, SNAPS, s_rows + t * FFT_N), TAG, "mean");
        vTaskDelay(1); /* let IDLE run (task watchdog) */
    }
    int64_t us = esp_timer_get_time() - t0;
    /* Median across tunes per bin removes signals that sit at one RF frequency. */
    int16_t col[TUNES];
    for (unsigned j = 0; j < FFT_N; j++) {
        for (int t = 0; t < TUNES; t++) {
            col[t] = s_rows[t * FFT_N + j];
        }
        qsort(col, TUNES, sizeof(col[0]), cmp_i16);
        s_cdb[j] = (int16_t)((col[TUNES / 2 - 1] + col[TUNES / 2]) / 2);
    }
    int32_t g[GROUPS];
    const unsigned per = FFT_N / GROUPS;
    for (int i = 0; i < GROUPS; i++) {
        int32_t s = 0;
        for (unsigned j = i * per; j < (i + 1) * per; j++) {
            s += (j >= FFT_N / 2 - 2 && j <= FFT_N / 2 + 2) ? s_cdb[FFT_N / 2 + 3] : s_cdb[j];
        }
        g[i] = s / (int32_t)per;
    }
    int32_t ref = (g[GROUPS / 2 - 2] + g[GROUPS / 2 + 1]) / 2;
    char line[GROUPS * 7 + 1];
    size_t n = 0;
    for (int i = 0; i < GROUPS; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%+5.1f ", (g[i] - ref) / 100.0f);
    }
    ESP_LOGI(TAG, "shape %2u MS/s bw=%2u (ref %.1f dB, %d tunes x %d snaps in %" PRId64 " ms): %s",
             rx_rate_hz(rate) / 1000000, bw, ref / 100.0f, TUNES, SNAPS, us / 1000, line);
    return ESP_OK;
}

/* A bin counts only if it rises in every cycle, so Wi-Fi bursts that happen
 * to land in one window do not. */
static esp_err_t lcd_spur_test(unsigned mhz)
{
    int16_t *idle = s_rows, *busy = s_rows + FFT_N, *rise = s_rows + 2 * FFT_N;
    ESP_RETURN_ON_ERROR(rx_tune(mhz), TAG, "tune");
    ESP_RETURN_ON_ERROR(rx_set_gain_manual(GAIN), TAG, "gain");
    for (unsigned j = 0; j < FFT_N; j++) {
        rise[j] = INT16_MAX;
    }
    int32_t fl_idle = 0, fl_busy = 0;
    for (int c = 0; c < LCD_CYCLES; c++) {
        s_lcd_busy = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_RETURN_ON_ERROR(mean_cdb(RX_RATE_40M, LCD_SNAPS, idle), TAG, "idle");
        s_lcd_busy = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_RETURN_ON_ERROR(mean_cdb(RX_RATE_40M, LCD_SNAPS, busy), TAG, "busy");
        fl_idle += dsp_percentile(idle, FFT_N / 8, FFT_N - FFT_N / 8, FFT_N / 2, DC_GUARD, 20, s_scratch);
        fl_busy += dsp_percentile(busy, FFT_N / 8, FFT_N - FFT_N / 8, FFT_N / 2, DC_GUARD, 20, s_scratch);
        for (unsigned j = 0; j < FFT_N; j++) {
            int d = busy[j] - idle[j];
            rise[j] = d < rise[j] ? (int16_t)d : rise[j];
        }
        vTaskDelay(1);
    }
    s_lcd_busy = false;
    ESP_LOGI(TAG, "LCD test %u MHz (%s), %d cycles: floor idle %.2f dB, LCD busy %.2f dB", mhz,
             s_quiet ? "radio-quiet ON" : "radio-quiet OFF", LCD_CYCLES, fl_idle / LCD_CYCLES / 100.0f,
             fl_busy / LCD_CYCLES / 100.0f);
    int found = 0;
    for (int r = 0; r < 8; r++) {
        int best = -1, best_d = 300; /* rises of at least 3 dB in every cycle */
        for (unsigned j = 0; j < FFT_N; j++) {
            if ((j < FFT_N / 2 - 2 || j > FFT_N / 2 + 2) && rise[j] >= best_d) {
                best_d = rise[j];
                best = (int)j;
            }
        }
        if (best < 0) {
            break;
        }
        found++;
        float off = ((int)best - FFT_N / 2) * 40.0f / FFT_N;
        ESP_LOGI(TAG, "  rise >= %+.1f dB in all cycles at %+.2f MHz (%.2f MHz)", best_d / 100.0f, off, mhz + off);
        rise[best] = INT16_MIN;
    }
    if (!found) {
        ESP_LOGI(TAG, "  no bin rose >= 3 dB in every cycle");
    }
    return ESP_OK;
}

esp_err_t characterize_run(void)
{
    ESP_LOGW(TAG, "CONFIG_JD_CHARACTERIZE: running receiver characterisation (~1 min)");
    int16_t *window = heap_caps_malloc(FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *twiddle = heap_caps_malloc(FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *work = heap_caps_malloc(2 * FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint16_t *order = heap_caps_malloc(FFT_N * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_iq = heap_caps_malloc(2 * FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_power = heap_caps_malloc(FFT_N * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_sum = heap_caps_malloc(FFT_N * sizeof(uint64_t), MALLOC_CAP_SPIRAM);
    s_cdb = heap_caps_malloc(FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_scratch = heap_caps_malloc(FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_rows = heap_caps_malloc(TUNES * FFT_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint16_t *frame = heap_caps_malloc(BOARD_LCD_W * 20 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    esp_err_t err = ESP_ERR_NO_MEM;
    TaskHandle_t hammer = NULL;
    const UBaseType_t prio = uxTaskPriorityGet(NULL);
    if (!(window && twiddle && work && order && s_iq && s_power && s_sum && s_cdb && s_scratch && s_rows && frame)) {
        ESP_LOGE(TAG, "buffers");
        goto out;
    }
    dsp_fft_init(&s_fft, FFT_N, window, twiddle, work, order);
    for (unsigned i = 0; i < BOARD_LCD_W * 20; i++) {
        frame[i] = (uint16_t)(i * 2654435761u >> 16); /* lots of bit toggling */
    }

    static const rx_rate_t rates[] = {RX_RATE_20M, RX_RATE_40M, RX_RATE_80M};
    static const unsigned bws[] = {0, 34, 20};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (unsigned b = 0; b < sizeof(bws) / sizeof(bws[0]); b++) {
            if ((err = filter_shape(rates[r], bws[b])) != ESP_OK) {
                goto out;
            }
        }
    }
    if ((err = rx_set_bw(0)) != ESP_OK) {
        goto out;
    }
    /* Like the real sweep (priority 10 vs LVGL 4), the capturing task must
     * outrank the LCD writer, or the writer re-takes the radio-quiet token
     * every time the ISR frees it and starves the capture. */
    vTaskPrioritySet(NULL, HAMMER_PRIO + 1);
    if (xTaskCreate(lcd_hammer_task, "lcd_hammer", 3072, frame, HAMMER_PRIO, &hammer) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    static const unsigned freqs[] = {2412, 2437, 2462, 5500};
    for (int q = 0; q < 2; q++) {
        s_quiet = q == 1;
        for (unsigned i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
            if ((err = lcd_spur_test(freqs[i])) != ESP_OK) {
                goto out;
            }
        }
    }
out:
    if (hammer) {
        s_lcd_busy = false;
        vTaskDelay(pdMS_TO_TICKS(200)); /* hammer back to idle before deleting it */
        vTaskDelete(hammer);
    }
    vTaskPrioritySet(NULL, prio);
    heap_caps_free(window);
    heap_caps_free(twiddle);
    heap_caps_free(work);
    heap_caps_free(order);
    heap_caps_free(s_iq);
    heap_caps_free(s_power);
    heap_caps_free(s_sum);
    heap_caps_free(s_cdb);
    heap_caps_free(s_scratch);
    heap_caps_free(s_rows);
    heap_caps_free(frame);
    ESP_LOGW(TAG, "characterisation done: %s", esp_err_to_name(err));
    return err;
}
