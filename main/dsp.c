/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Window, FFT and power code follow ESPARGOS esp-sdr common/spectrum.c and
 * reuse its Q15 kernel (upstream/common/spectrum_fft.h) verbatim.
 */
#include <math.h>
#include <stddef.h>
#include "dsp.h"
#include "spectrum_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static unsigned bitrev(unsigned v, unsigned bits)
{
    unsigned r = 0;
    for (unsigned b = 0; b < bits; b++) {
        r = (r << 1) | ((v >> b) & 1u);
    }
    return r;
}

static int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

int dsp_fft_init(dsp_fft_t *f, unsigned n, int16_t *window, int16_t *twiddle, int16_t *work, uint16_t *order)
{
    if (n < 256 || n > DSP_MAX_N || (n & (n - 1)) || !window || !twiddle || !work || !order) {
        return -1;
    }
    f->n = n;
    f->log2n = 0;
    while ((1u << f->log2n) < n) {
        f->log2n++;
    }
    f->window = window;
    f->twiddle = twiddle;
    f->work = work;
    f->order = order;
    /* FFT output is bit-reversed; centre DC at n/2. */
    for (unsigned m = 0; m < n; m++) {
        order[m] = (uint16_t)bitrev((m + n / 2) & (n - 1), f->log2n);
    }
    /* Same window as upstream spectrum.c: 32767 * Hann. */
    for (unsigned j = 0; j < n; j++) {
        window[j] = (int16_t)lrintf(16383.5f * (1.0f - cosf(2.0f * (float)M_PI * j / n)));
    }
    /* esp-dsp dsps_gen_w_r2_sc16() followed by dsps_bit_rev_sc16_ansi(w, n/2). */
    unsigned half = n / 2, hbits = f->log2n - 1;
    for (unsigned i = 0; i < half; i++) {
        unsigned k = bitrev(i, hbits);
        float e = 2.0f * (float)M_PI * k / n;
        twiddle[2 * i] = (int16_t)(32767.0f * cosf(e));
        twiddle[2 * i + 1] = (int16_t)(32767.0f * sinf(e));
    }
    return 0;
}

static void no_poll(void *ctx)
{
    (void)ctx;
}

void dsp_power_spectrum(dsp_fft_t *f, const int16_t *iq, uint32_t *power)
{
    const unsigned n = f->n;
    int16_t *x = f->work;
    for (unsigned j = 0; j < n; j++) {
        x[2 * j] = clamp16((iq[2 * j] * f->window[j]) >> 9);
        x[2 * j + 1] = clamp16((iq[2 * j + 1] * f->window[j]) >> 9);
    }
    spectrum_fft_poll(x, n, f->twiddle, no_poll, NULL);
    for (unsigned m = 0; m < n; m++) {
        unsigned at = f->order[m];
        int32_t re = x[2 * at], im = x[2 * at + 1];
        power[m] = (uint32_t)(re * re) + (uint32_t)(im * im);
    }
}

int16_t dsp_cdb(uint32_t p)
{
    /* log2(1 + i/16) in Q12, as in upstream spectrum_math.h. */
    static const uint16_t log2_q12[17] = {0,    358,  696,  1016, 1319, 1607, 1882, 2144, 2396,
                                          2637, 2869, 3092, 3307, 3515, 3715, 3908, 4096};
    if (!p) {
        return 0;
    }
    unsigned e = 31u - (unsigned)__builtin_clz(p);
    uint32_t norm = (p << (31u - e)) >> 23; /* 256..511 */
    unsigned slot = (norm - 256u) >> 4, frac = norm & 15u;
    uint32_t l2 = (e << 12) + log2_q12[slot] + ((log2_q12[slot + 1] - log2_q12[slot]) * frac + 8) / 16;
    /* 10*log10(2) = 3.0103 dB per octave -> 301.03 cdB; l2 is Q12. */
    return (int16_t)(((uint64_t)l2 * 30103u + (409600u / 2)) / 409600u);
}

void dsp_to_cdb(const uint32_t *power, int16_t *cdb, unsigned lo, unsigned hi)
{
    for (unsigned j = lo; j < hi; j++) {
        cdb[j] = dsp_cdb(power[j]);
    }
}

static int in_guard(unsigned j, unsigned dc, unsigned guard)
{
    return guard && j + guard > dc && j < dc + guard; /* |j - dc| < guard */
}

int16_t dsp_percentile(const int16_t *cdb, unsigned lo, unsigned hi, unsigned dc, unsigned dc_guard,
                       unsigned pct, int16_t *scratch)
{
    unsigned m = 0;
    for (unsigned j = lo; j < hi; j++) {
        if (!in_guard(j, dc, dc_guard)) {
            scratch[m++] = cdb[j];
        }
    }
    if (!m) {
        return 0;
    }
    unsigned want = (m - 1) * (pct > 100 ? 100 : pct) / 100;
    /* Quickselect (Hoare partition) for the want-th smallest value. */
    unsigned left = 0, right = m - 1;
    while (left < right) {
        int16_t pivot = scratch[(left + right) / 2];
        unsigned i = left, j = right;
        while (i <= j) {
            while (scratch[i] < pivot) {
                i++;
            }
            while (scratch[j] > pivot) {
                j--;
            }
            if (i <= j) {
                int16_t t = scratch[i];
                scratch[i] = scratch[j];
                scratch[j] = t;
                i++;
                if (j == 0) {
                    break;
                }
                j--;
            }
        }
        if (want <= j) {
            right = j;
        } else if (want >= i) {
            left = i;
        } else {
            break;
        }
    }
    return scratch[want];
}

unsigned dsp_peak_bin(const int16_t *cdb, unsigned lo, unsigned hi, unsigned dc, unsigned dc_guard)
{
    unsigned best = lo;
    int16_t best_v = INT16_MIN;
    for (unsigned j = lo; j < hi; j++) {
        if (!in_guard(j, dc, dc_guard) && cdb[j] > best_v) {
            best_v = cdb[j];
            best = j;
        }
    }
    return best;
}

void dsp_dc_notch(uint32_t *power, unsigned n, unsigned guard)
{
    if (!guard || guard + 2 > n / 2) {
        return;
    }
    const unsigned dc = n / 2;
    uint64_t sum = (uint64_t)power[dc - guard] + power[dc - guard - 1] + power[dc + guard] + power[dc + guard + 1];
    uint32_t fill = (uint32_t)(sum / 4);
    for (unsigned j = dc - guard + 1; j < dc + guard; j++) {
        power[j] = fill;
    }
}

void dsp_reduce(const uint32_t *avg, const uint32_t *max, unsigned fft_first, unsigned decim, unsigned count,
                int16_t *avg_cdb, int16_t *max_cdb)
{
    for (unsigned k = 0; k < count; k++) {
        const unsigned j0 = fft_first + k * decim;
        uint64_t sum = 0;
        uint32_t peak = 0;
        for (unsigned d = 0; d < decim; d++) {
            sum += avg[j0 + d];
            if (max[j0 + d] > peak) {
                peak = max[j0 + d];
            }
        }
        avg_cdb[k] = dsp_cdb((uint32_t)(sum / decim));
        max_cdb[k] = dsp_cdb(peak);
    }
}
