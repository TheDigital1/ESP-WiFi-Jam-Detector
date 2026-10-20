/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Board support for the Waveshare ESP32-C5-Touch-LCD-3.5: shared I2C bus,
 * CH32V006 IO expander (LCD/touch reset, backlight, audio PA), AXP2101
 * status and battery, ST7796 LCD in landscape, FT6336 touch and the BOOT
 * button.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#define BOARD_LCD_W 480 /* landscape */
#define BOARD_LCD_H 320

typedef enum {
    BOARD_ORIENT_LANDSCAPE = 0,  /* MADCTL MV */
    BOARD_ORIENT_LANDSCAPE_FLIP, /* MADCTL MV|MX|MY, rotated 180 degrees */
} board_orient_t;

typedef struct {
    bool pressed;
    uint16_t x, y;         /* landscape coordinates for the current orientation */
    uint16_t raw_x, raw_y; /* controller coordinates (portrait, 320x480) */
} board_touch_t;

/* Bring up I2C, expander, PMU log, LCD (backlight left off), touch and BOOT button. */
esp_err_t board_init(board_orient_t orient);

/* Log every responding 7-bit address on the shared I2C bus. */
esp_err_t board_i2c_scan(void);

/* Log AXP2101 identity and status. Reads registers only, never writes. */
esp_err_t board_pmu_log(void);

typedef struct {
    bool vbus;           /* USB power present */
    bool battery;        /* battery connected */
    bool charging;
    int percent;         /* fuel gauge, -1 without a battery */
    uint16_t battery_mv;
} board_power_t;

esp_err_t board_power_read(board_power_t *out);

/* NS4150B speaker amplifier (expander P5). */
esp_err_t board_pa_enable(bool on);

/* The shared I2C bus, for drivers that attach their own devices (ES8311). */
i2c_master_bus_handle_t board_i2c_bus(void);

esp_err_t board_backlight_set(uint8_t percent);

esp_err_t board_lcd_set_orient(board_orient_t orient);
board_orient_t board_lcd_orient(void);

/* Blocking draw of big-endian RGB565 pixels into [x0,x1) x [y0,y1).
 * Returns once the DMA transfer has finished, so the buffer can be reused.
 * The buffer may be in internal DMA RAM or in PSRAM (direct PSRAM DMA). */
esp_err_t board_lcd_draw(int x0, int y0, int x1, int y1, const void *px);

/* Called from the SPI transfer-done ISR when an async draw has finished. */
typedef void (*board_lcd_done_cb_t)(void *ctx);

/* Start a draw and return; done(ctx) runs in ISR context once the buffer may
 * be reused. Waits (up to 500 ms) while a capture holds the radio-quiet token. */
esp_err_t board_lcd_draw_async(int x0, int y0, int x1, int y1, const void *px, board_lcd_done_cb_t done, void *ctx);

/* Radio-quiet window: blocks until no LCD transfer is in flight and keeps new
 * ones from starting until board_rf_quiet_end(). Wrap each capture in it. */
esp_err_t board_rf_quiet_begin(uint32_t timeout_ms);
void board_rf_quiet_end(void);

/* RGB565 value already byte-swapped for the panel's big-endian bus order. */
static inline uint16_t board_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

esp_err_t board_touch_read(board_touch_t *t);

/* BOOT button level, true while held (active low). */
bool board_boot_pressed(void);
