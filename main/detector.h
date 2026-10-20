/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Detector task: collects the sweep's per-tune segments into whole-band
 * spectra and runs detect.c on each completed sweep, below the sweep's
 * priority. Logs every state change with its metrics. Learned baselines are
 * stored (settings.c) and restored at the next boot.
 */
#pragma once

#include "esp_err.h"
#include "bandplan.h"
#include "detect.h"

/* Call after sweep_start() (needs the band plans). */
esp_err_t detector_start(const detect_config_t *cfg);
esp_err_t detector_get_status(band_id_t band, detect_status_t *out);
/* Per-bin baseline (cdB); out needs the band's n_bins entries. */
esp_err_t detector_get_baseline(band_id_t band, int16_t *out);
/* New thresholds for both bands; baselines are kept. */
esp_err_t detector_set_config(const detect_config_t *cfg);
/* Forget both baselines (stored ones too) and learn again (takes effect on the next sweep). */
void detector_relearn(void);
