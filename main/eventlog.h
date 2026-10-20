/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CSV log of detector state changes on the TF card (/sdcard/jd_events.csv).
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
    uint64_t free_kb;
} eventlog_status_t;

/* Mounts the card (shares SPI2 with the LCD; board_init() first) and starts
 * the writer task. Returns ESP_OK without a card (logging off). */
esp_err_t eventlog_start(void);
/* Detector task: queue one state change; never blocks. */
void eventlog_post(band_id_t band, const detect_event_t *ev);
void eventlog_get_status(eventlog_status_t *out);
