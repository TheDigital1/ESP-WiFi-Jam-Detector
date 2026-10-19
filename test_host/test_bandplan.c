/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host unit tests for main/bandplan.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include "bandplan.h"

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

/* Every grid bin is owned by exactly one slice, slices are contiguous and in
 * order, and each grid bin's FFT bins sit inside the slice's usable span. */
static void check_plan(band_id_t band, const bp_config_t *cfg, unsigned want_slices)
{
    band_plan_t p;
    if (bp_build(&p, band, cfg) != 0) {
        CHECK(0, "band %d: build failed", band);
        return;
    }
    CHECK(p.n_slices == want_slices, "%s: %u slices, want %u", p.name, p.n_slices, want_slices);
    CHECK(bp_bin_khz(&p, p.n_bins) >= p.stop_khz, "%s: grid ends at %u kHz before stop %u", p.name,
          bp_bin_khz(&p, p.n_bins), p.stop_khz);
    unsigned next = 0;
    const double bin_hz = (double)cfg->rate_hz / cfg->fft_n;
    const double half_usable_bins = cfg->usable_hz / 2.0 / bin_hz;
    for (unsigned s = 0; s < p.n_slices; s++) {
        CHECK(p.slice_first_bin[s] == next, "%s slice %u starts at %u, want %u", p.name, s, p.slice_first_bin[s], next);
        next += p.slice_bins[s];
        double lo = p.slice_fft_first[s] - cfg->fft_n / 2.0;
        double hi = lo + (double)p.slice_bins[s] * cfg->decim;
        /* Allow one grid bin of rounding beyond the nominal usable span. */
        CHECK(lo >= -half_usable_bins - cfg->decim && hi <= half_usable_bins + cfg->decim,
              "%s slice %u (%u MHz): FFT bins %.0f..%.0f outside usable +/-%.1f", p.name, s, p.center_mhz[s], lo, hi,
              half_usable_bins);
        /* The grid bin's frequency must match the FFT bin frequency to within one FFT bin. */
        double grid_off_hz = (double)bp_bin_khz(&p, p.slice_first_bin[s]) * 1000.0 - p.center_mhz[s] * 1e6;
        CHECK(abs((int)(grid_off_hz / bin_hz - lo)) <= 1, "%s slice %u: grid/FFT misaligned (%.2f vs %.2f bins)", p.name, s,
              grid_off_hz / bin_hz, lo);
    }
    CHECK(next == p.n_bins, "%s: slices own %u bins, grid has %u", p.name, next, p.n_bins);
    printf("%s: %u slices, %u grid bins of %u Hz, centres %u..%u MHz\n", p.name, p.n_slices, p.n_bins, p.res_hz,
           p.center_mhz[0], p.center_mhz[p.n_slices - 1]);
}

int main(void)
{
    /* Default plan: 80 MS/s, 1024-point FFT, keep the central 48 MHz, 4 FFT bins per grid bin. */
    const bp_config_t def = {.rate_hz = 80000000, .fft_n = 1024, .usable_hz = 48000000, .decim = 4};
    check_plan(BAND_24, &def, 2);
    check_plan(BAND_5, &def, 16);
    /* Brief's original suggestion: 40 MS/s, central 75 % = 30 MHz (decim 8 keeps the 312.5 kHz grid). */
    const bp_config_t alt = {.rate_hz = 40000000, .fft_n = 1024, .usable_hz = 30000000, .decim = 8};
    check_plan(BAND_24, &alt, 3);
    check_plan(BAND_5, &alt, 25);

    band_plan_t p;
    bp_build(&p, BAND_5, &def);
    CHECK(bp_label_of(&p, 5180000) == 0, "5180 -> U-NII-1");
    CHECK(bp_label_of(&p, 5400000) == -1, "5400 is between U-NII-2A and 2C");
    CHECK(bp_label_of(&p, 5745000) == 3, "5745 -> U-NII-3");

    const bp_config_t too_fine = {.rate_hz = 40000000, .fft_n = 1024, .usable_hz = 30000000, .decim = 4};
    CHECK(bp_build(&p, BAND_5, &too_fine) == -1, "4768 grid bins exceed BP_MAX_BINS and must be rejected");

    const bp_config_t bad = {.rate_hz = 80000000, .fft_n = 1024, .usable_hz = 48500000, .decim = 4};
    CHECK(bp_build(&p, BAND_24, &bad) == -1, "non-integer MHz step must be rejected");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_bandplan: all checks passed\n");
    return 0;
}
