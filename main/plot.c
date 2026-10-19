/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include "plot.h"

int plot_y(const plot_buf_t *b, const plot_style_t *st, int16_t v)
{
    int32_t range = st->max_cdb - st->min_cdb;
    int32_t y = (int32_t)(st->max_cdb - v) * (int32_t)(b->h - 1) / (range > 0 ? range : 1);
    return y < 0 ? 0 : (y >= (int32_t)b->h ? (int)b->h - 1 : (int)y);
}

/* Vertical span joining the previous column's point to this one, so steep
 * edges stay connected. */
static void set_span(int16_t *lo, int16_t *hi, int prev, int y)
{
    int a = prev < 0 ? y : prev;
    *lo = (int16_t)(a < y ? a : y);
    *hi = (int16_t)(a < y ? y : a);
}

void plot_prepare(plot_frame_t *f, const int16_t *avg, const int16_t *hold, const int16_t *base,
                  const uint16_t *grid_x, unsigned n_grid_x)
{
    const plot_buf_t *b = f->geom;
    const plot_style_t *st = f->st;
    unsigned gi = 0;
    int py_avg = -1, py_hold = -1, py_base = -1;
    for (unsigned x = 0; x < b->w; x++) {
        plot_col_t *c = &f->cols[x];
        while (gi < n_grid_x && grid_x[gi] < x) {
            gi++;
        }
        c->vgrid = gi < n_grid_x && grid_x[gi] == x;
        c->fill_from = INT16_MAX;
        c->avg_lo = c->hold_lo = c->base_lo = 1;
        c->avg_hi = c->hold_hi = c->base_hi = 0; /* empty */
        if (avg[x] != PLOT_NONE) {
            int ya = plot_y(b, st, avg[x]);
            c->fill_from = (int16_t)(ya + 1);
            set_span(&c->avg_lo, &c->avg_hi, py_avg, ya);
            py_avg = ya;
        } else {
            py_avg = -1;
        }
        if (hold && hold[x] != PLOT_NONE) {
            int yh = plot_y(b, st, hold[x]);
            set_span(&c->hold_lo, &c->hold_hi, py_hold, yh);
            py_hold = yh;
        } else {
            py_hold = -1;
        }
        if (base && base[x] != PLOT_NONE) {
            int yb = plot_y(b, st, base[x]);
            set_span(&c->base_lo, &c->base_hi, py_base, yb);
            py_base = yb;
        } else {
            py_base = -1;
        }
    }
    /* Grid at every multiple of the step strictly inside the range, top down. */
    f->n_grid_rows = 0;
    if (st->grid_step_cdb > 0) {
        int32_t v = (st->max_cdb / st->grid_step_cdb) * st->grid_step_cdb;
        if (v >= st->max_cdb) {
            v -= st->grid_step_cdb;
        }
        for (; v > st->min_cdb && f->n_grid_rows < PLOT_MAX_GRID_ROWS; v -= st->grid_step_cdb) {
            f->grid_rows[f->n_grid_rows++] = (int16_t)plot_y(b, st, (int16_t)v);
        }
    }
}

void plot_rows(const plot_frame_t *f, unsigned y0, unsigned rows, uint16_t *px, unsigned stride)
{
    const plot_style_t *st = f->st;
    const unsigned w = f->geom->w;
    unsigned gr = 0;
    for (unsigned r = 0; r < rows; r++) {
        const int16_t yy = (int16_t)(y0 + r);
        while (gr < f->n_grid_rows && f->grid_rows[gr] < yy) {
            gr++;
        }
        const bool hgrid = gr < f->n_grid_rows && f->grid_rows[gr] == yy;
        uint16_t *row = px + r * stride;
        /* Layering (bottom to top): background/grid, fill, base, hold, avg. */
        for (unsigned x = 0; x < w; x++) {
            const plot_col_t *c = &f->cols[x];
            uint16_t v = (hgrid || c->vgrid) ? st->grid : st->bg;
            if (yy >= c->fill_from) {
                v = st->fill;
            }
            if (yy >= c->base_lo && yy <= c->base_hi) {
                v = st->base;
            }
            if (yy >= c->hold_lo && yy <= c->hold_hi) {
                v = st->hold;
            }
            if (yy >= c->avg_lo && yy <= c->avg_hi) {
                v = st->avg;
            }
            row[x] = v;
        }
    }
}

void plot_spectrum(const plot_buf_t *b, const plot_style_t *st, const int16_t *avg, const int16_t *hold,
                   const int16_t *base, const uint16_t *grid_x, unsigned n_grid_x, plot_col_t *cols)
{
    plot_frame_t f = {.geom = b, .st = st, .cols = cols};
    plot_prepare(&f, avg, hold, base, grid_x, n_grid_x);
    plot_rows(&f, 0, b->h, b->px, b->stride);
}
