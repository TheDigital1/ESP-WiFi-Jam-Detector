/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host unit tests for main/dsp.c. Build and run: make -C test_host
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static int16_t window[DSP_MAX_N], twiddle[DSP_MAX_N], work[2 * DSP_MAX_N], iq[2 * DSP_MAX_N];
static uint16_t order[DSP_MAX_N];
static uint32_t power[DSP_MAX_N];
static int16_t cdb[DSP_MAX_N], scratch[DSP_MAX_N];

/* Complex tone at bin offset k from DC, amplitude a (10-bit units). */
static void make_tone(unsigned n, int k, double a, double noise)
{
    for (unsigned j = 0; j < n; j++) {
        double ph = 2.0 * M_PI * k * (double)j / n;
        double ni = noise * ((double)rand() / RAND_MAX - 0.5), nq = noise * ((double)rand() / RAND_MAX - 0.5);
        iq[2 * j] = (int16_t)lrint(a * cos(ph) + ni);
        iq[2 * j + 1] = (int16_t)lrint(a * sin(ph) + nq);
    }
}

static void test_tone_position(unsigned n)
{
    dsp_fft_t f;
    CHECK(dsp_fft_init(&f, n, window, twiddle, work, order) == 0, "init n=%u", n);
    const int offsets[] = {1, 7, 100, -100, (int)n / 2 - 3, -(int)n / 2 + 3};
    for (unsigned t = 0; t < sizeof(offsets) / sizeof(offsets[0]); t++) {
        int k = offsets[t];
        make_tone(n, k, 400, 4);
        dsp_power_spectrum(&f, iq, power);
        dsp_to_cdb(power, cdb, 0, n);
        unsigned peak = dsp_peak_bin(cdb, 0, n, n / 2, 0);
        CHECK(peak == (unsigned)((int)n / 2 + k), "n=%u tone %+d: peak bin %u, want %d", n, k, peak, (int)n / 2 + k);
        /* Hann leaks into the two neighbours only; 10 bins away must be >40 dB down. */
        unsigned far = (unsigned)((int)n / 2 + k + (k > 0 ? -10 : 10));
        CHECK(cdb[peak] - cdb[far] > 4000, "n=%u tone %+d: sidelobe only %d cdB down", n, k, cdb[peak] - cdb[far]);
    }
}

static void test_dc(void)
{
    const unsigned n = 1024;
    dsp_fft_t f;
    dsp_fft_init(&f, n, window, twiddle, work, order);
    for (unsigned j = 0; j < n; j++) {
        iq[2 * j] = 200;
        iq[2 * j + 1] = -150;
    }
    dsp_power_spectrum(&f, iq, power);
    dsp_to_cdb(power, cdb, 0, n);
    CHECK(dsp_peak_bin(cdb, 0, n, n / 2, 0) == n / 2, "DC not at centre bin");
    unsigned off = dsp_peak_bin(cdb, 0, n, n / 2, 2);
    CHECK(cdb[n / 2] - cdb[off] > 4000, "DC guard: strongest non-DC bin only %d cdB below DC",
          cdb[n / 2] - cdb[off]);
}

static void test_full_scale_level(void)
{
    const unsigned n = 1024;
    dsp_fft_t f;
    dsp_fft_init(&f, n, window, twiddle, work, order);
    make_tone(n, 50, 511, 0);
    dsp_power_spectrum(&f, iq, power);
    /* Input maps 511 -> ~2^15, Hann coherent gain 0.5, FFT scaled by 1/n:
     * |X| ~ 32704 * 0.5 -> power ~ 2.67e8 -> ~84.3 dB. */
    int16_t level = dsp_cdb(power[n / 2 + 50]);
    CHECK(level > 8300 && level < 8550, "full-scale tone at %d cdB, expected ~8430", level);
}

static void test_cdb_accuracy(void)
{
    double worst = 0;
    for (uint64_t p = 1; p <= 0x80000000u; p = p * 3 / 2 + 1) {
        double want = 1000.0 * log10((double)p);
        double err = fabs(dsp_cdb((uint32_t)p) - want);
        if (err > worst) {
            worst = err;
        }
    }
    CHECK(worst <= 3.0, "cdB worst error %.2f cdB", worst);
    CHECK(dsp_cdb(0) == 0, "cdb(0)");
}

static void test_dc_notch_and_reduce(void)
{
    const unsigned n = 1024;
    for (unsigned j = 0; j < n; j++) {
        power[j] = 1000;
    }
    power[n / 2] = 5000000;
    power[n / 2 - 1] = power[n / 2 + 1] = 90000;
    dsp_dc_notch(power, n, 3);
    for (unsigned j = n / 2 - 2; j <= n / 2 + 2; j++) {
        CHECK(power[j] == 1000, "notch bin %u = %u", j, power[j]);
    }
    static uint32_t mx[DSP_MAX_N];
    for (unsigned j = 0; j < n; j++) {
        power[j] = 100 * (j % 4 + 1); /* mean of each 4-group = 250 */
        mx[j] = j;
    }
    static int16_t a[64], m[64];
    dsp_reduce(power, mx, 100, 4, 10, a, m);
    for (int k = 0; k < 10; k++) {
        CHECK(abs(a[k] - dsp_cdb(250)) <= 1, "reduce avg %d: %d", k, a[k]);
        CHECK(m[k] == dsp_cdb(100 + 4 * k + 3), "reduce max %d: %d", k, m[k]);
    }
    /* Grid min over two snapshots: the second is lower only in grid bin 3. */
    static uint32_t gmin[64];
    dsp_reduce_min(power, 100, 4, 10, gmin, 1);
    for (unsigned j = 0; j < n; j++) {
        power[j] = j >= 112 && j < 116 ? 10 : 1000;
    }
    dsp_reduce_min(power, 100, 4, 10, gmin, 0);
    for (int k = 0; k < 10; k++) {
        CHECK(gmin[k] == (k == 3 ? 10u : 250u), "reduce min %d: %u", k, (unsigned)gmin[k]);
    }
}

static int cmp16(const void *a, const void *b)
{
    return *(const int16_t *)a - *(const int16_t *)b;
}

static void test_percentile(void)
{
    static int16_t ref[DSP_MAX_N];
    for (int trial = 0; trial < 500; trial++) {
        unsigned n = 16 + (unsigned)(rand() % 1000), dc = (unsigned)rand() % n, guard = (unsigned)rand() % 4;
        unsigned pct = (unsigned)(rand() % 101);
        for (unsigned j = 0; j < n; j++) {
            cdb[j] = (int16_t)(rand() % (trial % 2 ? 50 : 9000)); /* odd trials: many duplicates */
        }
        unsigned m = 0;
        for (unsigned j = 0; j < n; j++) {
            if (!(guard && j + guard > dc && j < dc + guard)) {
                ref[m++] = cdb[j];
            }
        }
        qsort(ref, m, sizeof(int16_t), cmp16);
        int16_t want = ref[(m - 1) * pct / 100];
        int16_t got = dsp_percentile(cdb, 0, n, dc, guard, pct, scratch);
        CHECK(got == want, "percentile trial %d (n=%u pct=%u): got %d want %d", trial, n, pct, got, want);
    }
}

int main(void)
{
    srand(1);
    test_tone_position(256);
    test_tone_position(1024);
    test_tone_position(2048);
    test_dc();
    test_full_scale_level();
    test_cdb_accuracy();
    test_percentile();
    test_dc_notch_and_reduce();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_dsp: all checks passed\n");
    return 0;
}
