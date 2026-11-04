/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Card set-up as Waveshare's BSP bsp_sdcard_sdspi_mount(): SDSPI on the LCD's
 * SPI2 bus, CS GPIO9, no CD/WP, not formatted on failure. Each write runs
 * inside the radio-quiet window, like an LCD transfer: card traffic on the
 * shared bus must not overlap a capture.
 *
 * Waterfall images are written the same way, a band of rows per window with
 * a gap between windows, so a ~300 KB file does not stop the sweep for the
 * whole write. Every chunk ends with fflush(): no card I/O may be left over
 * for outside the window.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "board_pins.h"
#include "eventlog.h"
#include "settings.h"

static const char *TAG = "eventlog";

#define MOUNT      "/sdcard"
#define PATH       MOUNT "/jd_events.csv"
#define QUEUE_LEN  16
#define TASK_PRIO  2
#define WRITE_PRIO 12    /* while holding the radio-quiet token, as a capture does: the
                            sweep and LCD wait on it, so nothing may pre-empt the write */
#define TASK_STACK 4096
#define QUIET_MS   500
#define IMG_DIR    MOUNT "/waterfall"
#define IMG_ROWS   32    /* image rows per radio-quiet window (30 KB at 480 px) */
#define IMG_GAP_MS 300   /* between windows: lets a sweep and the LCD through */
#define IMG_MIN_KB 2048  /* card space kept free */

typedef enum { ITEM_EVENT, ITEM_IMAGE } item_kind_t;

typedef struct {
    item_kind_t kind;
    union {
        struct {
            band_id_t band;
            detect_event_t ev;
        };
        eventlog_image_t img;
    };
} item_t;

static QueueHandle_t s_queue;
static sdmmc_card_t *s_card;
static eventlog_status_t s_st;

static const char HEADER[] =
    "boot,uptime_s,band,from,to,kind,floor_db,continuous_db,max_cell_db,max_cell_mhz,flatness_db,wide_pct,"
    "continuous_pct,occupancy_pct,duty_pct,clipped_pct,evidence,evidence_duty_pct,window_sweeps,cw_mhz,cw_bins,"
    "cw_db,swept_mhz,swept_mhz_per_s,swept_steps,swept_cover_pct\n";

static int write_row(FILE *f, const item_t *it)
{
    const detect_event_t *e = &it->ev;
    const detect_metrics_t *m = &e->m;
    return fprintf(f,
                   "%" PRIu32 ",%.1f,%s,%s,%s,%s,%.2f,%.2f,%.2f,%.1f,%.2f,%u,%u,%u,%u,%u,0x%02x,%u,%u,%.2f,%u,%.2f,"
                   "%.2f,%.2f,%u,%u\n",
                   settings_boot(), e->t_ms / 1000.0, it->band == BAND_24 ? "2.4" : "5", detect_state_name(e->from),
                   detect_state_name(e->to), detect_kind_name(e->kind), m->floor_delta_cdb / 100.0,
                   m->cont_delta_cdb / 100.0, m->floor_delta_max_cdb / 100.0, m->floor_delta_max_khz / 1000.0,
                   m->flatness_cdb / 100.0, m->wide_pct, m->cont_pct, m->occupancy_pct, m->duty_pct, m->clipped_pct,
                   m->evidence, m->evidence_duty_pct, m->window_sweeps, m->cw_khz / 1000.0, m->cw_bins,
                   m->cw_excess_cdb / 100.0, m->swept_khz / 1000.0, m->swept_khz_per_s / 1000.0, m->swept_steps,
                   m->swept_cover_pct);
}

static void update_free(void)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &free_b) == ESP_OK) {
        s_st.free_kb = free_b / 1024;
    }
}

/* Holds the radio-quiet token; false if the window was not granted. */
static bool quiet_begin(void)
{
    if (board_rf_quiet_begin(QUIET_MS) != ESP_OK) {
        return false;
    }
    vTaskPrioritySet(NULL, WRITE_PRIO);
    return true;
}

static void quiet_end(void)
{
    vTaskPrioritySet(NULL, TASK_PRIO);
    board_rf_quiet_end();
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* 16-bit BMP with RGB565 bit fields: the canvas pixels go out unchanged
 * (little-endian), bottom row first as BMP wants. */
static bool write_image(const eventlog_image_t *img)
{
    char path[96];
    snprintf(path, sizeof(path), IMG_DIR "/%s", img->name);
    if (s_st.free_kb < IMG_MIN_KB) {
        ESP_LOGW(TAG, "card nearly full: %s not saved", path);
        return false;
    }
    /* Rows are padded to 4 bytes in the file. */
    const uint32_t row_b = (uint32_t)img->w * 2, pad = (4 - row_b % 4) % 4, off = 14 + 40 + 12,
                   size = off + (row_b + pad) * img->h;
    static const uint8_t zeros[3];
    uint8_t hdr[14 + 40 + 12] = {'B', 'M'};
    put_le(hdr + 2, size, 4);
    put_le(hdr + 10, off, 4);
    put_le(hdr + 14, 40, 4);
    put_le(hdr + 18, img->w, 4);
    put_le(hdr + 22, img->h, 4);
    put_le(hdr + 26, 1, 2);
    put_le(hdr + 28, 16, 2);
    put_le(hdr + 30, 3, 4); /* BI_BITFIELDS */
    put_le(hdr + 34, (row_b + pad) * img->h, 4);
    put_le(hdr + 38, 2835, 4); /* 72 dpi */
    put_le(hdr + 42, 2835, 4);
    put_le(hdr + 54, 0xf800, 4);
    put_le(hdr + 58, 0x07e0, 4);
    put_le(hdr + 62, 0x001f, 4);

    const int64_t t0 = esp_timer_get_time();
    unsigned windows = 0;
    if (!quiet_begin()) {
        return false;
    }
    mkdir(IMG_DIR, 0775); /* fails harmlessly when it exists */
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (int y = img->h - 1; ok && y >= 0;) {
        for (int n = 0; ok && n < IMG_ROWS && y >= 0; n++, y--) {
            ok = fwrite(img->px + (size_t)y * img->stride, 1, row_b, f) == row_b &&
                 fwrite(zeros, 1, pad, f) == pad;
        }
        ok = ok && fflush(f) == 0;
        windows++;
        if (ok && y >= 0) {
            quiet_end();
            vTaskDelay(pdMS_TO_TICKS(IMG_GAP_MS));
            while (!quiet_begin()) {
                /* The file is open: card I/O must wait for a window, however long. */
            }
        }
    }
    if (f && fclose(f) != 0) {
        ok = false;
    }
    update_free();
    quiet_end();
    if (ok) {
        s_st.images++;
        ESP_LOGI(TAG, "saved %s (%" PRIu32 " KB, %u windows, %lld ms)", path, size / 1024, windows,
                 (esp_timer_get_time() - t0) / 1000);
    }
    return ok;
}

static void writer_task(void *arg)
{
    (void)arg;
    item_t it;
    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (it.kind == ITEM_IMAGE) {
            if (!write_image(&it.img)) {
                s_st.errors++;
                ESP_LOGW(TAG, "waterfall image %s not saved", it.img.name);
            }
            heap_caps_free(it.img.px);
            continue;
        }
        if (board_rf_quiet_begin(QUIET_MS) != ESP_OK) {
            s_st.errors++;
            continue;
        }
        vTaskPrioritySet(NULL, WRITE_PRIO);
        struct stat sb;
        const bool fresh = stat(PATH, &sb) != 0 || sb.st_size == 0;
        FILE *f = fopen(PATH, "a");
        bool ok = f != NULL;
        if (ok && fresh) {
            ok = fputs(HEADER, f) >= 0;
        }
        /* Drain whatever else is queued in the same window. */
        while (ok) {
            ok = write_row(f, &it) > 0;
            s_st.written += ok;
            if (xQueuePeek(s_queue, &it, 0) != pdTRUE || it.kind != ITEM_EVENT) {
                break; /* an image waits for its own windows */
            }
            xQueueReceive(s_queue, &it, 0);
        }
        if (f && fclose(f) != 0) {
            ok = false;
        }
        update_free();
        vTaskPrioritySet(NULL, TASK_PRIO);
        board_rf_quiet_end();
        if (!ok) {
            s_st.errors++;
            ESP_LOGW(TAG, "write to %s failed", PATH);
        }
    }
}

esp_err_t eventlog_start(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_LCD_SPI_HOST; /* bus already set up by board_init(), with MISO */
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_CS;
    slot.host_id = BOARD_LCD_SPI_HOST;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    ESP_RETURN_ON_ERROR(board_rf_quiet_begin(QUIET_MS), TAG, "bus");
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mount, &s_card);
    board_rf_quiet_end();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no TF card (%s): event log off. With a card in, check DIP switch JP8 pin 2 is ON.",
                 esp_err_to_name(err));
        return ESP_OK;
    }
    s_st.mounted = true;
    update_free();
    ESP_LOGI(TAG, "TF card %s, %" PRIu64 " MB, %" PRIu64 " MB free; logging to %s (boot %" PRIu32 ")",
             s_card->cid.name, (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
             s_st.free_kb / 1024, PATH, settings_boot());
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(item_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(writer_task, "eventlog", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void eventlog_post(band_id_t band, const detect_event_t *ev)
{
    if (!s_queue) {
        return;
    }
    const item_t it = {.kind = ITEM_EVENT, .band = band, .ev = *ev};
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        s_st.dropped++;
    }
}

bool eventlog_post_image(const eventlog_image_t *img)
{
    if (!s_queue) {
        return false;
    }
    item_t it = {.kind = ITEM_IMAGE, .img = *img};
    return xQueueSend(s_queue, &it, 0) == pdTRUE;
}

bool eventlog_card_ready(void)
{
    return s_queue && s_st.free_kb >= IMG_MIN_KB;
}

void eventlog_get_status(eventlog_status_t *out)
{
    *out = s_st;
}
