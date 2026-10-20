/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Card set-up as Waveshare's BSP bsp_sdcard_sdspi_mount(): SDSPI on the LCD's
 * SPI2 bus, CS GPIO9, no CD/WP, not formatted on failure. Each write runs
 * inside the radio-quiet window, like an LCD transfer: card traffic on the
 * shared bus must not overlap a capture.
 */
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
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

typedef struct {
    band_id_t band;
    detect_event_t ev;
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

static void writer_task(void *arg)
{
    (void)arg;
    item_t it;
    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE) {
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
            if (xQueueReceive(s_queue, &it, 0) != pdTRUE) {
                break;
            }
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
    const item_t it = {.band = band, .ev = *ev};
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        s_st.dropped++;
    }
}

void eventlog_get_status(eventlog_status_t *out)
{
    *out = s_st;
}
