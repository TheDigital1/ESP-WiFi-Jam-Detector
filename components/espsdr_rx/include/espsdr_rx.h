/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal receive-only API over the ESPARGOS esp-sdr ESP32-C5 snapshot
 * receiver. All calls must come from one task (the sweep task). No function
 * here transmits; the radio stays in Wi-Fi NULL mode with promiscuous RX.
 *
 * While this receiver is in use, normal Wi-Fi (scan/connect/AP) must not be
 * used: it retunes the radio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot sample-rate codes (esp-sdr docs/protocol.md, C5 rows). */
typedef enum {
    RX_RATE_80M = 0,
    RX_RATE_40M = 1,
    RX_RATE_20M = 2,
    RX_RATE_10M = 3,
    RX_RATE_8M = 4,
    RX_RATE_4M = 5,
} rx_rate_t;

#define RX_MAX_SAMPLES 16380u /* upstream IQ_WORDS: one snapshot, the bank minus overrun canaries */
#define RX_IQ_FULL_SCALE 511  /* 10-bit signed samples: -512..511 */

typedef struct {
    uint32_t capture_us; /* time the dump engine ran */
    unsigned clipped;    /* samples with |I| or |Q| at the 10-bit rail */
    unsigned gain_code;  /* receiver gain index reported in the first word (bits 20+) */
} rx_capture_info_t;

/* Wi-Fi/PHY bring-up exactly as upstream app_main(). NVS must be initialised. */
esp_err_t rx_init(void);

/* Tune the LO. Above 3000 MHz the C5 selects its 5 GHz path. A new frequency
 * runs upstream's receive-only DC recalibration (no TX loopback). */
esp_err_t rx_tune(unsigned mhz);
unsigned rx_frequency(void);

esp_err_t rx_set_gain_manual(unsigned index);
esp_err_t rx_set_gain_auto(void);
unsigned rx_gain_max(void);

/* Analog bandwidth in MHz, 11..48, or 0 for the widest (open) setting. */
esp_err_t rx_set_bw(unsigned mhz);

/* Take one snapshot of n samples (256..RX_MAX_SAMPLES) and unpack it into
 * iq_out as interleaved int16 I,Q pairs holding the raw 10-bit values. */
esp_err_t rx_capture(unsigned n, rx_rate_t rate, int16_t *iq_out, rx_capture_info_t *info);

static inline unsigned rx_rate_hz(rx_rate_t rate)
{
    static const unsigned hz[] = {80000000, 40000000, 20000000, 10000000, 8000000, 4000000};
    return (unsigned)rate < sizeof(hz) / sizeof(hz[0]) ? hz[rate] : 0;
}

#ifdef __cplusplus
}
#endif
