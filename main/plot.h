/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Column renderer for the spectrum view: writes RGB565 pixels straight into
 * a canvas buffer, one column per frequency point. Much cheaper than LVGL's
 * anti-aliased chart lines on this FPU-less, PSRAM-backed target.
 * No ESP-IDF or LVGL headers (host-tested).
 */
#pragma once

#include <stdint.h>

#define PLOT_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* Same colour with bytes swapped, for panels that take big-endian RGB565. */
#define PLOT_RGB565_BE(r, g, b) ((uint16_t)((PLOT_RGB565(r, g, b) >> 8) | (PLOT_RGB565(r, g, b) << 8)))
#define PLOT_NONE            INT16_MIN /* "no data" value in point arrays */

typedef struct {
    uint16_t *px; /* RGB565, native byte order */
    unsigned w, h, stride; /* stride in pixels */
} plot_buf_t;

typedef struct {
    int16_t min_cdb, max_cdb; /* value range mapped to bottom..top */
    int16_t grid_step_cdb;    /* horizontal grid every N cdB, 0 = none */
    uint16_t bg, grid, fill, avg, hold, base;
} plot_style_t;

/* Per-column spans, worked out before the row-major fill. */
typedef struct {
    int16_t fill_from;          /* first fill row, or INT16_MAX for none */
    int16_t avg_lo, avg_hi;     /* avg line rows (lo > hi: none) */
    int16_t hold_lo, hold_hi;
    int16_t base_lo, base_hi;
    uint8_t vgrid;
} plot_col_t;

#define PLOT_MAX_GRID_ROWS 32

/* One frame's layout: per-column spans plus horizontal grid rows. */
typedef struct {
    const plot_buf_t *geom; /* w, h used for scaling; px/stride unused here */
    const plot_style_t *st;
    plot_col_t *cols;       /* caller scratch, w entries */
    int16_t grid_rows[PLOT_MAX_GRID_ROWS];
    unsigned n_grid_rows;
} plot_frame_t;

/* Work out the frame. avg/hold/base hold w values each (cdB); base and hold
 * may be NULL. grid_x lists columns (ascending) with a vertical grid line. */
void plot_prepare(plot_frame_t *f, const int16_t *avg, const int16_t *hold, const int16_t *base,
                  const uint16_t *grid_x, unsigned n_grid_x);

/* Fill rows [y0, y0 + rows) of the frame into px (row 0 of px = frame row
 * y0), row-major. Colours are written exactly as given in the style, so a
 * style with byte-swapped colours renders straight into panel order. */
void plot_rows(const plot_frame_t *f, unsigned y0, unsigned rows, uint16_t *px, unsigned stride);

/* Convenience: prepare and fill the whole buffer b. */
void plot_spectrum(const plot_buf_t *b, const plot_style_t *st, const int16_t *avg, const int16_t *hold,
                   const int16_t *base, const uint16_t *grid_x, unsigned n_grid_x, plot_col_t *cols);

/* Row for value v (clamped to the buffer), as used by plot_spectrum. */
int plot_y(const plot_buf_t *b, const plot_style_t *st, int16_t v);
