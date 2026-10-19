/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "board.h"
#include "lvgl_port.h"

static const char *TAG = "lvgl";

#define BUF_ROWS   20 /* one chunk = 480 x 20 px = 19.2 KB, ~2.6 ms on the 60 MHz bus */
/* Above the sweep (10). LVGL work here is short and mostly waiting on LCD DMA
 * (>= 5 ms sleep per loop, ~10-20 ms CPU per frame); below the sweep it was
 * starved for latency by the 25 ms 5 GHz retunes and managed ~1 frame/s. */
#define TASK_PRIO  11
#define TASK_STACK 6144
#define MIN_SLEEP  5
#define MAX_SLEEP  50

static SemaphoreHandle_t s_lock;

/* ---- LV_USE_CUSTOM_MALLOC: keep LVGL's heap out of internal SRAM ---- */

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

/* ---- display ---- */

static void flush_done(void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx); /* ISR context; only clears a flag */
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    lv_draw_sw_rgb565_swap(px, lv_area_get_size(area)); /* panel takes big-endian RGB565 */
    esp_err_t err = board_lcd_draw_async(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px, flush_done, disp);
    if (err != ESP_OK) {
        lv_display_flush_ready(disp); /* drop the frame rather than wedge LVGL */
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    board_touch_t t;
    if (board_touch_read(&t) == ESP_OK && t.pressed) {
        data->point.x = t.x;
        data->point.y = t.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t sleep = MAX_SLEEP;
        if (lvp_lock(portMAX_DELAY)) {
            sleep = lv_timer_handler();
            lvp_unlock();
        }
        sleep = sleep < MIN_SLEEP ? MIN_SLEEP : (sleep > MAX_SLEEP ? MAX_SLEEP : sleep);
        vTaskDelay(pdMS_TO_TICKS(sleep));
    }
}

bool lvp_lock(uint32_t timeout_ms)
{
    TickType_t t = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE;
}

void lvp_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

esp_err_t lvp_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");

    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t *disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_NO_MEM, TAG, "display");
    const size_t bytes = BOARD_LCD_W * BUF_ROWS * sizeof(uint16_t);
    /* PSRAM, DMA'd directly by the LCD SPI (measured as fast as internal in M1;
     * internal buffers made LVGL only ~1 fps faster, not worth 38 KB of SRAM). */
    void *b1 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    void *b2 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(b1 && b2, ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *touch = lv_indev_create();
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_NO_MEM, TAG, "touch indev");
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);

    ESP_RETURN_ON_FALSE(xTaskCreate(lvgl_task, "lvgl", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "lvgl task");
    ESP_LOGI(TAG, "LVGL %d.%d.%d up, %dx%d, 2 x %u B draw buffers in PSRAM", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, BOARD_LCD_W, BOARD_LCD_H, (unsigned)bytes);
    return ESP_OK;
}
