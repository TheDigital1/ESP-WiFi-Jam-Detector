/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host unit tests for main/plot.c.
 */
#include <stdio.h>
#include "plot.h"

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

#define W 40
#define H 91

static uint16_t px[W * H];
static plot_col_t cols[W];

int main(void)
{
    const plot_buf_t b = {.px = px, .w = W, .h = H, .stride = W};
    const plot_style_t st = {
        .min_cdb = 0, .max_cdb = 9000, .grid_step_cdb = 1000,
        .bg = 1, .grid = 2, .fill = 3, .avg = 4, .hold = 5, .base = 6,
    };
    CHECK(plot_y(&b, &st, 9000) == 0, "top");
    CHECK(plot_y(&b, &st, 0) == H - 1, "bottom");
    CHECK(plot_y(&b, &st, 4500) == 45, "middle: %d", plot_y(&b, &st, 4500));
    CHECK(plot_y(&b, &st, -500) == H - 1 && plot_y(&b, &st, 12000) == 0, "clamp");

    int16_t avg[W], hold[W];
    for (int x = 0; x < W; x++) {
        avg[x] = 3000;  /* row 60 */
        hold[x] = 6000; /* row 30 */
    }
    avg[20] = 8000;     /* spike: row 10 */
    avg[30] = PLOT_NONE;
    const uint16_t grid_x[] = {5, 25};
    plot_spectrum(&b, &st, avg, hold, NULL, grid_x, 2, cols);

    CHECK(px[60 * W + 0] == 4, "avg line at row 60");
    CHECK(px[80 * W + 0] == 3, "fill below avg");
    CHECK(px[41 * W + 1] == 1, "background between traces (got %u)", px[41 * W + 1]);
    CHECK(px[30 * W + 0] == 5, "hold line at row 30");
    CHECK(px[10 * W + 20] == 4, "spike top drawn");
    CHECK(px[35 * W + 20] == 4, "spike joined to previous column (vertical span)");
    CHECK(px[50 * W + 21] == 4, "falling edge joined");
    CHECK(px[40 * W + 5] == 2 && px[40 * W + 25] == 2, "vertical grid columns");
    CHECK(px[plot_y(&b, &st, 7000) * W + 2] == 2, "horizontal grid row");
    CHECK(px[79 * W + 30] == 1 && px[61 * W + 30] == 1, "no-data column left empty (rows 40/80 are grid)");

    /* Rendering in strips gives the same pixels as one pass. */
    static uint16_t strips[W * H];
    plot_frame_t f = {.geom = &b, .st = &st, .cols = cols};
    plot_prepare(&f, avg, hold, NULL, grid_x, 2);
    for (unsigned y0 = 0; y0 < H; y0 += 7) {
        unsigned rows = y0 + 7 <= H ? 7 : H - y0;
        plot_rows(&f, y0, rows, strips + y0 * W, W);
    }
    int diff = 0;
    for (unsigned i = 0; i < W * H; i++) {
        diff += strips[i] != px[i];
    }
    CHECK(diff == 0, "strip rendering differs in %d pixels", diff);
    CHECK(PLOT_RGB565_BE(0xff, 0, 0) == 0x00f8, "big-endian red");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_plot: all checks passed\n");
    return 0;
}
