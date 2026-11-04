/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp-wifi-jammer-detector: receive-only RF interference / possible-jamming
 * detector for the Waveshare ESP32-C5-Touch-LCD-3.5.
 *
 * Band sweep, per-band interference detection against a learned baseline,
 * status / spectrum + waterfall / settings screens, settings, gains and
 * baselines kept in flash, alarm beep, TF-card event log, battery level.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "audio.h"
#include "board.h"
#include "characterize.h"
#include "detector.h"
#include "eventlog.h"
#include "espsdr_rx.h"
#include "lvgl_port.h"
#include "settings.h"
#include "sweep.h"
#include "ui.h"

static const char *TAG = "app";

#define CAPTURE_GUARD 0x40820000u /* upstream sram_guard.ld */

extern char _bss_end;

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "[%s] headroom below capture guard %u B; internal heap free %u (largest %u, min ever %u), "
             "PSRAM free %u",
             when, (unsigned)(CAPTURE_GUARD - (uintptr_t)&_bss_end),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Sweep task: store the gains and learn baselines at them. */
static void on_autorange(const unsigned gain[BAND_COUNT])
{
    settings_t s;
    settings_get(&s);
    for (int b = 0; b < BAND_COUNT; b++) {
        s.gain[b] = (uint8_t)gain[b];
    }
    esp_err_t err = settings_set(&s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "storing gains: %s", esp_err_to_name(err));
    }
    detector_relearn();
}

static esp_err_t start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
    ESP_RETURN_ON_ERROR(settings_init(), TAG, "settings");
    settings_t set;
    settings_get(&set);

    ESP_RETURN_ON_ERROR(board_init(BOARD_ORIENT_LANDSCAPE), TAG, "board");
    ESP_RETURN_ON_ERROR(eventlog_start(), TAG, "event log");
    esp_err_t aerr = audio_init();
    if (aerr != ESP_OK) {
        ESP_LOGW(TAG, "no alarm beep: %s", esp_err_to_name(aerr)); /* not fatal */
    }
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(rx_init(), TAG, "receiver");
    ESP_LOGI(TAG, "rx_init %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);

#if CONFIG_JD_CHARACTERIZE
    ESP_RETURN_ON_ERROR(characterize_run(), TAG, "characterize");
#endif

    /* 80 MS/s, widest analog filter, central 48 MHz per tune: the flattest
     * usable span measured in M3 (CONFIG_JD_CHARACTERIZE).
     * Gains come from the first boot's auto-range (stored), or Settings. */
    sweep_config_t cfg = {
        .rate = RX_RATE_80M,
        .bw_mhz = 0,
        .usable_hz = 48000000,
        .snapshots = 4,
        .yield_pct = 10, /* for detect + IDLE; the UI task outranks the sweep */
        .on_autorange = on_autorange,
    };
    for (int b = 0; b < BAND_COUNT; b++) {
        cfg.weight[b] = set.weight[b];
        cfg.gain[b] = set.gain[b] == SETTINGS_GAIN_NONE ? SWEEP_GAIN_AUTO : set.gain[b];
    }
    ESP_RETURN_ON_ERROR(sweep_start(&cfg), TAG, "sweep");
    ESP_RETURN_ON_ERROR(detector_start(&set.det), TAG, "detector");
    ESP_RETURN_ON_ERROR(lvp_init(), TAG, "lvgl");
    ESP_RETURN_ON_ERROR(ui_init(), TAG, "ui");
    ESP_RETURN_ON_ERROR(board_backlight_set(set.brightness), TAG, "backlight");
    return ESP_OK;
}

#if CONFIG_JD_CPU_STATS
static void cpu_stats_task(void *arg)
{
    static char buf[1024];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        vTaskGetRunTimeStats(buf);
        ESP_LOGI(TAG, "CPU since boot (task, abs, %%):\n%s", buf);
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "esp-wifi-jammer-detector (receive only)");
    esp_err_t err = start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup failed: %s", esp_err_to_name(err));
        return;
    }
    log_memory("running");
#if CONFIG_JD_CPU_STATS
    xTaskCreate(cpu_stats_task, "cpu_stats", 3072, NULL, 1, NULL);
#endif
    /* Hourly, for leaks that only show after days of uptime. */
    for (unsigned h = 1;; h++) {
        vTaskDelay(pdMS_TO_TICKS(3600 * 1000));
        char when[16];
        snprintf(when, sizeof(when), "%u h", h);
        log_memory(when);
    }
}
