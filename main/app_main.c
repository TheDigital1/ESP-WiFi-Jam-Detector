/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp-wifi-jammer-detector, M1 board bring-up test. No radio.
 *
 * - Logs chip, PSRAM size, heap and headroom below the capture-SRAM guard.
 * - Draws a test pattern (colour bars, corner markers, grey ramp) and times
 *   a full-screen draw from internal DMA RAM and from PSRAM.
 * - Touch draws a yellow dot under the finger and logs coordinates.
 * - BOOT short press flips the landscape orientation 180 degrees; a long
 *   press (>= 1 s) is logged (it will re-learn the baseline later).
 */
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "board.h"

static const char *TAG = "app";

#define CHUNK_ROWS     20
#define CHUNK_PX       (BOARD_LCD_W * CHUNK_ROWS)
#define DOT            8
#define LONG_PRESS_US  1000000
#define DEBOUNCE_US    30000
#define CAPTURE_GUARD  0x40820000u /* upstream sram_guard.ld */

extern char _iram_end, _data_end, _bss_end;

static uint16_t *s_buf_int;   /* internal DMA-capable */
static uint16_t *s_buf_psram; /* PSRAM, sent with psram_dma_direct */

static void log_memory(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d core(s)", chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "PSRAM: %u KiB", (unsigned)(esp_psram_get_size() / 1024));
    ESP_LOGI(TAG, "static RAM: iram_end=%p data_end=%p bss_end=%p, headroom below capture guard %u bytes",
             &_iram_end, &_data_end, &_bss_end, (unsigned)(CAPTURE_GUARD - (uintptr_t)&_bss_end));
    ESP_LOGI(TAG, "heap internal free %u (largest %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static uint16_t pattern_px(int x, int y)
{
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    };
    const int m = 50, pad = 5;
    if (y < pad + m || y >= BOARD_LCD_H - pad - m) {
        bool left = x >= pad && x < pad + m, right = x >= BOARD_LCD_W - pad - m && x < BOARD_LCD_W - pad;
        bool top = y >= pad && y < pad + m, bottom = y >= BOARD_LCD_H - pad - m && y < BOARD_LCD_H - pad;
        if (top && left) {
            return board_rgb565(255, 255, 255);
        }
        if (top && right) {
            return board_rgb565(255, 0, 0);
        }
        if (bottom && left) {
            return board_rgb565(0, 255, 0);
        }
        if (bottom && right) {
            return board_rgb565(0, 0, 255);
        }
        if (bottom && x >= 2 * pad + m && x < BOARD_LCD_W - 2 * pad - m) {
            int span = BOARD_LCD_W - 4 * pad - 2 * m;
            uint8_t g = (uint8_t)((x - 2 * pad - m) * 255 / (span - 1));
            return board_rgb565(g, g, g);
        }
        return board_rgb565(0, 0, 0);
    }
    const uint8_t *c = bars[x * 8 / BOARD_LCD_W];
    return board_rgb565(c[0], c[1], c[2]);
}

/* Draw the full pattern through one chunk buffer; returns elapsed microseconds. */
static esp_err_t draw_pattern(uint16_t *buf, int64_t *us)
{
    int64_t t0 = esp_timer_get_time();
    for (int y0 = 0; y0 < BOARD_LCD_H; y0 += CHUNK_ROWS) {
        for (int y = 0; y < CHUNK_ROWS; y++) {
            for (int x = 0; x < BOARD_LCD_W; x++) {
                buf[y * BOARD_LCD_W + x] = pattern_px(x, y0 + y);
            }
        }
        ESP_RETURN_ON_ERROR(board_lcd_draw(0, y0, BOARD_LCD_W, y0 + CHUNK_ROWS, buf), TAG, "pattern draw failed");
    }
    *us = esp_timer_get_time() - t0;
    return ESP_OK;
}

/* Pure transfer time: the buffer is filled once, then sent for a whole frame. */
static esp_err_t time_transfer(uint16_t *buf, int64_t *us)
{
    int64_t t0 = esp_timer_get_time();
    for (int y0 = 0; y0 < BOARD_LCD_H; y0 += CHUNK_ROWS) {
        ESP_RETURN_ON_ERROR(board_lcd_draw(0, y0, BOARD_LCD_W, y0 + CHUNK_ROWS, buf), TAG, "timing draw failed");
    }
    *us = esp_timer_get_time() - t0;
    return ESP_OK;
}

static esp_err_t benchmark_and_draw(void)
{
    int64_t us = 0;
    ESP_RETURN_ON_ERROR(time_transfer(s_buf_int, &us), TAG, "internal timing");
    ESP_LOGI(TAG, "full-frame transfer, internal DMA buffer: %" PRId64 " us (%.1f fps)", us, 1e6 / us);
    if (s_buf_psram) {
        ESP_RETURN_ON_ERROR(time_transfer(s_buf_psram, &us), TAG, "PSRAM timing");
        ESP_LOGI(TAG, "full-frame transfer, PSRAM buffer (direct DMA): %" PRId64 " us (%.1f fps)", us, 1e6 / us);
        ESP_RETURN_ON_ERROR(draw_pattern(s_buf_psram, &us), TAG, "PSRAM pattern");
        ESP_LOGI(TAG, "pattern render+draw via PSRAM: %" PRId64 " us", us);
    }
    ESP_RETURN_ON_ERROR(draw_pattern(s_buf_int, &us), TAG, "internal pattern");
    ESP_LOGI(TAG, "pattern render+draw via internal: %" PRId64 " us", us);
    return ESP_OK;
}

static esp_err_t draw_dot(int x, int y)
{
    for (int i = 0; i < DOT * DOT; i++) {
        s_buf_int[i] = board_rgb565(255, 255, 0);
    }
    x = x < DOT / 2 ? 0 : (x > BOARD_LCD_W - DOT / 2 ? BOARD_LCD_W - DOT : x - DOT / 2);
    y = y < DOT / 2 ? 0 : (y > BOARD_LCD_H - DOT / 2 ? BOARD_LCD_H - DOT : y - DOT / 2);
    return board_lcd_draw(x, y, x + DOT, y + DOT, s_buf_int);
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp-wifi-jammer-detector M1 board bring-up (no radio)");
    log_memory();

    s_buf_int = heap_caps_malloc(CHUNK_PX * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf_psram = heap_caps_aligned_calloc(64, CHUNK_PX, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_buf_int) {
        ESP_LOGE(TAG, "no internal DMA memory for the draw buffer");
        return;
    }
    if (!s_buf_psram) {
        ESP_LOGW(TAG, "no DMA-capable PSRAM buffer; skipping the PSRAM benchmark");
    }

    esp_err_t err = board_init(BOARD_ORIENT_LANDSCAPE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
        return;
    }
    err = benchmark_and_draw();
    if (err == ESP_OK) {
        err = board_backlight_set(100);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display test failed: %s", esp_err_to_name(err));
        return;
    }
    log_memory();
    ESP_LOGI(TAG, "ready: touch the screen, press BOOT to flip, hold BOOT >= 1 s for long press");

    bool btn_level = false, btn_stable = false, long_logged = false;
    int64_t btn_change = 0, btn_down = 0, last_touch_log = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();

        bool level = board_boot_pressed();
        if (level != btn_level) {
            btn_level = level;
            btn_change = now;
        } else if (level != btn_stable && now - btn_change >= DEBOUNCE_US) {
            btn_stable = level;
            if (btn_stable) {
                btn_down = now;
                long_logged = false;
                ESP_LOGI(TAG, "BOOT pressed");
            } else if (!long_logged) {
                board_orient_t next = board_lcd_orient() == BOARD_ORIENT_LANDSCAPE ? BOARD_ORIENT_LANDSCAPE_FLIP
                                                                                   : BOARD_ORIENT_LANDSCAPE;
                ESP_LOGI(TAG, "BOOT short press -> orientation %d", next);
                int64_t us;
                err = board_lcd_set_orient(next);
                if (err == ESP_OK) {
                    err = draw_pattern(s_buf_int, &us);
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "orientation change failed: %s", esp_err_to_name(err));
                }
            }
        }
        if (btn_stable && !long_logged && now - btn_down >= LONG_PRESS_US) {
            long_logged = true;
            ESP_LOGI(TAG, "BOOT long press");
        }

        board_touch_t t;
        err = board_touch_read(&t);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch read: %s", esp_err_to_name(err));
        } else if (t.pressed) {
            err = draw_dot(t.x, t.y);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "dot draw: %s", esp_err_to_name(err));
            }
            if (now - last_touch_log >= 100000) {
                last_touch_log = now;
                ESP_LOGI(TAG, "touch raw=(%u,%u) -> screen=(%u,%u)", t.raw_x, t.raw_y, t.x, t.y);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
