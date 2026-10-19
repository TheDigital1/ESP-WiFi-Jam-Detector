/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modified from ESPARGOS esp-sdr @f86da81, main/families/c5_c6_c61/receiver.c
 * (Copyright ESPARGOS contributors, GPL-3.0-or-later). The unmodified file is
 * kept beside it as upstream/families/c5_c6_c61/receiver.c.orig.
 *
 * Changes from upstream:
 * - ESP32-C5 paths only; C6/C61 branches and the SAMPLE_RATE_PROBE /
 *   FILTER_REGISTER_PROBE / RING_PROBE / C5_TUNE_PROBE diagnostics removed.
 * - The serial command server, USB/UART transport, GPIO commands and
 *   on-chip spectrum are removed. app_main()'s Wi-Fi/PHY bring-up became
 *   rx_init(); the BANDWIDTH command became rx_set_bw(); FREQ became rx_tune();
 *   GAIN MANUAL/HARDWARE became rx_set_gain_manual()/rx_set_gain_auto().
 * - acquire_iq() reports errors as esp_err_t instead of protocol replies, and
 *   rx_capture() unpacks the snapshot out of the capture bank.
 * - Logging stays enabled except for the "phy" tag (upstream silences all
 *   logging). The PHY library logs one line per capture from adctrig(); over
 *   the USB console that cost ~2 ms per snapshot and much of the CPU.
 * prepare_rx(), the capture sequence, sentinels and SRAM ownership handling
 * are unchanged.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_phy_cert_test.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "chip.h"
#include "rx_tuning.h"
#include "rx_recalibration.h"
#include "rx_bandwidth.h"
#include "espsdr_rx.h"

static const char *TAG = "espsdr_rx";

extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
extern void phy_chip_set_chan(unsigned, unsigned);
extern void phy_rx_filter_mode(unsigned);
static unsigned frequency_mhz = 2412;
static bool rx_ready;
static unsigned rx_channel_mode;
static int rx_filter = -1; /* -1 restores the PHY-calibrated automatic mode. */
static int rx_analog_filter = -1;
#define RX_FILTER_REG 6u
extern unsigned phy_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void phy_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
/* RX-only analog capacitance. Preserve PHY calibration between snapshots,
 * across retunes; zero is the widest tested code, not bypass. */
static void rx_analog_apply(unsigned saved[2]) {
    for (unsigned j = 0; j < 2; j++) {
        saved[j] = phy_chip_i2c_readReg(0x67, 1, RX_FILTER_REG + j);
        if (rx_analog_filter >= 0)
            phy_i2c_writeReg(0x67, 1, RX_FILTER_REG + j, (saved[j] & ~63u) | (unsigned)rx_analog_filter);
    }
}
static void rx_analog_restore(const unsigned saved[2]) {
    if (rx_analog_filter >= 0)
        for (unsigned j = 0; j < 2; j++) phy_i2c_writeReg(0x67, 1, RX_FILTER_REG + j, saved[j]);
}

/* Required by the stock RF test archive; no shell is exposed. */
int cmd_parse(char *cmd, char *name, int *argc, char **argv) {
    (void)cmd;
    (void)name;
    (void)argc;
    (void)argv;
    return -1;
}
/* burst_gain.h's protocol handler (gain_command) is unused here. */
static void reply(const char *s) {
    (void)s;
}
#include "burst_gain.h"

static void prepare_rx(void) {
    static unsigned calibrated_mhz;
    if (rx_ready && !rx_recalibration_stale()) return;
    if (calibrated_mhz != frequency_mhz || rx_recalibration_stale()) {
        rx_recalibrate(frequency_mhz);
        calibrated_mhz = frequency_mhz;
    }
    phy_chip_set_chan(frequency_mhz, rx_channel_mode);
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
    if (rx_filter >= 0) phy_rx_filter_mode((unsigned)rx_filter);
    gain_apply();
    rx_ready = true;
}

static esp_err_t acquire_iq(unsigned n, unsigned divider, unsigned *capture_us) {
    prepare_rx();

    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = 0xa5a0055au;
    for (unsigned j = 0; j < 4; j++) IQ_BUFFER[n + j] = 0x5a5aa5a5u ^ j;
    unsigned analog_saved[2];
    rx_analog_apply(analog_saved);
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    bool done = true;
    int64_t start = esp_timer_get_time();
    /* Vendor selector 0 maps to raw source 15 and pulses the software trigger.
     * In particular, do NOT set CTRL bit 17 as in the C61 continuous backend:
     * on this C5 it produced only a short, incomplete snapshot. */
    done = stock_capture(n, divider);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(SRAM_OWNER_REG, owner);
    rx_analog_restore(analog_saved);
    if (!done) return ESP_ERR_TIMEOUT;
    for (unsigned j = 0; j < n; j++) {
        if (IQ_BUFFER[j] == 0xa5a0055au) return ESP_ERR_TIMEOUT; /* upstream "capture_timeout" */
    }
    for (unsigned j = 0; j < 4; j++) {
        if (IQ_BUFFER[n + j] != (0x5a5aa5a5u ^ j)) return ESP_ERR_INVALID_SIZE; /* "capture_overrun" */
    }
    *capture_us = elapsed;
    return ESP_OK;
}

esp_err_t rx_init(void) {
    esp_err_t e = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(e == ESP_OK || e == ESP_ERR_INVALID_STATE, e, TAG, "event loop: %s", esp_err_to_name(e));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_NULL), TAG, "set_mode NULL");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "set_promiscuous");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE), TAG, "set_channel");
    prepare_rx();
    /* The PHY logs every snapshot at INFO; see the header comment. */
    esp_log_level_set("phy", ESP_LOG_WARN);
    ESP_LOGI(TAG, "receiver ready at %u MHz, gain max %u", frequency_mhz, gain_max());
    return ESP_OK;
}

esp_err_t rx_tune(unsigned mhz) {
    ESP_RETURN_ON_FALSE(frequency_valid(mhz), ESP_ERR_INVALID_ARG, TAG, "frequency %u MHz out of range", mhz);
    frequency_mhz = mhz;
    rx_ready = false;
    prepare_rx();
    return ESP_OK;
}

unsigned rx_frequency(void) {
    return frequency_mhz;
}

esp_err_t rx_set_gain_manual(unsigned index) {
    /* Same range test as upstream gain_command(): GAIN_MIN..gain_max(). */
    ESP_RETURN_ON_FALSE(index - GAIN_MIN <= gain_max() - GAIN_MIN, ESP_ERR_INVALID_ARG, TAG,
                        "gain %u outside %u..%u", index, GAIN_MIN, gain_max());
    gain_mode = GAIN_MANUAL;
    gain_code = index;
    gain_apply();
    return ESP_OK;
}

esp_err_t rx_set_gain_auto(void) {
    gain_mode = GAIN_HARDWARE;
    gain_apply();
    return ESP_OK;
}

unsigned rx_gain_max(void) {
    return gain_max();
}

esp_err_t rx_set_bw(unsigned mhz) {
    ESP_RETURN_ON_FALSE(!mhz || (mhz >= RX_BANDWIDTH_MIN && mhz <= RX_BANDWIDTH_MAX), ESP_ERR_INVALID_ARG, TAG,
                        "bandwidth %u MHz outside %u..%u", mhz, RX_BANDWIDTH_MIN, RX_BANDWIDTH_MAX);
    rx_analog_filter = rx_bandwidth_dcap(mhz);
    unsigned mode = rx_bandwidth_phy_mode(mhz);
    if (mode != rx_channel_mode) {
        rx_channel_mode = mode;
        rx_ready = false;
        prepare_rx();
    }
    return ESP_OK;
}

esp_err_t rx_capture(unsigned n, rx_rate_t rate, int16_t *iq_out, rx_capture_info_t *info) {
    ESP_RETURN_ON_FALSE(n >= 256 && n <= RX_MAX_SAMPLES && iq_out && info, ESP_ERR_INVALID_ARG, TAG,
                        "bad capture args");
    ESP_RETURN_ON_FALSE(rx_rate_hz(rate), ESP_ERR_INVALID_ARG, TAG, "bad rate code %d", rate);
    unsigned us = 0;
    ESP_RETURN_ON_ERROR(acquire_iq(n, (unsigned)rate, &us), TAG, "capture at %u MHz", frequency_mhz);

    /* Same unpacking as upstream common/spectrum.c: I in bits 0-9, Q in 10-19. */
    unsigned clipped = 0;
    for (unsigned j = 0; j < n; j++) {
        uint32_t w = IQ_BUFFER[j];
        int32_t i = (int32_t)(w << 22) >> 22, q = (int32_t)(w << 12) >> 22;
        iq_out[2 * j] = (int16_t)i;
        iq_out[2 * j + 1] = (int16_t)q;
        clipped += (i >= RX_IQ_FULL_SCALE || i <= -RX_IQ_FULL_SCALE - 1 || q >= RX_IQ_FULL_SCALE ||
                    q <= -RX_IQ_FULL_SCALE - 1);
    }
    info->capture_us = us;
    info->clipped = clipped;
    info->gain_code = (uint8_t)(IQ_BUFFER[0] >> 20); /* as upstream spectrum.c: uint8_t gain = words[0] >> 20 */
    return ESP_OK;
}
