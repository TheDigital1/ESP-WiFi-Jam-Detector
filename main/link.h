/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB link to a host (the Home Assistant bridge): JSON lines on the USB
 * Serial/JTAG port that also carries the console log. Answers commands
 * (link_proto.h); while a host has sent one in the last minute, also sends
 * status (every 10 s and after state changes) and detector events. Writes go
 * out between captures (radio-quiet token) and are dropped, never waited for,
 * when the host does not read.
 */
#pragma once

#include "esp_err.h"
#include "bandplan.h"
#include "detect.h"

/* After ui_init() (commands change settings through the settings screen). */
esp_err_t link_start(void);

/* Detector task: queue one state change; never blocks. A status follows it. */
void link_post_event(band_id_t band, const detect_event_t *ev);
