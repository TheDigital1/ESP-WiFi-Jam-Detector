/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CSV log of detector state changes on the TF card (/sdcard/jd_events.csv),
 * and waterfall images around alarms (BMP files in /sdcard/waterfall).
 * Optional: without a card everything else runs as before.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bandplan.h"
#include "detect.h"

typedef struct {
    bool mounted;
    uint32_t written, dropped, errors; /* events since boot */
    uint32_t images;                   /* waterfall images since boot */
    uint64_t free_kb;
} eventlog_status_t;

/* Mounts the card (shares SPI2 with the LCD; board_init() first) and starts
 * the writer task. Returns ESP_OK without a card (logging off). */
esp_err_t eventlog_start(void);
/* Detector task: queue one state change; never blocks. */
void eventlog_post(band_id_t band, const detect_event_t *ev);
void eventlog_get_status(eventlog_status_t *out);

/* An RGB565 (little-endian) image to save as name.bmp. The writer frees px
 * with heap_caps_free() once written. */
typedef struct {
    uint16_t *px;
    uint16_t w, h, stride; /* stride in pixels */
    char name[64];
} eventlog_image_t;

/* True when a card is mounted with room for another image. */
bool eventlog_card_ready(void);
/* Queue an image; never blocks. On false the caller still owns px. */
bool eventlog_post_image(const eventlog_image_t *img);
