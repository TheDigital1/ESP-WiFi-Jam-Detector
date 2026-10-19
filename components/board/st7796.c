/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from espressif/esp_lcd_st7796 1.0.0 (the version Waveshare's BSP
 * pins) for ESP-IDF 6.x: panel_dev_config->color_space became rgb_ele_order,
 * the monolithic "driver" component is gone, and hardware reset is done by
 * the board code through the IO expander, so only software reset remains.
 * The vendor init sequence is unchanged. Distributed as part of a
 * GPL-3.0-or-later program; Apache-2.0 is compatible with GPLv3.
 */
#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_check.h"
#include "esp_log.h"
#include "st7796.h"

static const char *TAG = "st7796";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // current value of LCD_CMD_MADCTL
    uint8_t colmod_val; // current value of LCD_CMD_COLMOD
} st7796_panel_t;

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes; // 0xFF = end of list
} lcd_init_cmd_t;

static const lcd_init_cmd_t vendor_specific_init[] = {
    {0xf0, {0xc3}, 1},
    {0xf0, {0x96}, 1},
    {0xb4, {0x01}, 1},
    {0xb7, {0xc6}, 1},
    {0xe8, {0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33}, 8},
    {0xc1, {0x06}, 1},
    {0xc2, {0xa7}, 1},
    {0xc5, {0x18}, 1},
    {0xe0, {0xf0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2f, 0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b}, 14},
    {0xe1, {0xf0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2d, 0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b}, 14},
    {0xf0, {0x3c}, 1},
    {0xf0, {0x69}, 1},
    {0, {0}, 0xff},
};

static esp_err_t write_madctl(st7796_panel_t *st)
{
    return esp_lcd_panel_io_tx_param(st->io, LCD_CMD_MADCTL, (uint8_t[]) {st->madctl_val}, 1);
}

static esp_err_t panel_st7796_del(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    free(st);
    return ESP_OK;
}

static esp_err_t panel_st7796_reset(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st->io, LCD_CMD_SWRESET, NULL, 0), TAG, "SWRESET failed");
    vTaskDelay(pdMS_TO_TICKS(20)); // spec: wait at least 5 ms before the next command
    return ESP_OK;
}

static esp_err_t panel_st7796_init(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = st->io;

    // The panel is asleep after reset; leave sleep mode first.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(write_madctl(st), TAG, "MADCTL failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {st->colmod_val}, 1),
                        TAG, "COLMOD failed");
    for (int i = 0; vendor_specific_init[i].data_bytes != 0xff; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, vendor_specific_init[i].cmd, vendor_specific_init[i].data,
                                                      vendor_specific_init[i].data_bytes & 0x1F),
                            TAG, "vendor init cmd 0x%02x failed", vendor_specific_init[i].cmd);
    }
    return ESP_OK;
}

static esp_err_t panel_st7796_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "empty area");
    esp_lcd_panel_io_handle_t io = st->io;

    x_start += st->x_gap;
    x_end += st->x_gap;
    y_start += st->y_gap;
    y_end += st->y_gap;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF, ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF, ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "RASET failed");
    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * st->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_st7796_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    return esp_lcd_panel_io_tx_param(st->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st7796_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    st->madctl_val = mirror_x ? (st->madctl_val | LCD_CMD_MX_BIT) : (st->madctl_val & ~LCD_CMD_MX_BIT);
    st->madctl_val = mirror_y ? (st->madctl_val | LCD_CMD_MY_BIT) : (st->madctl_val & ~LCD_CMD_MY_BIT);
    return write_madctl(st);
}

static esp_err_t panel_st7796_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    st->madctl_val = swap_axes ? (st->madctl_val | LCD_CMD_MV_BIT) : (st->madctl_val & ~LCD_CMD_MV_BIT);
    return write_madctl(st);
}

static esp_err_t panel_st7796_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    st->x_gap = x_gap;
    st->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7796_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7796_panel_t *st = __containerof(panel, st7796_panel_t, base);
    return esp_lcd_panel_io_tx_param(st->io, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

esp_err_t st7796_new_panel(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg,
                           esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->reset_gpio_num < 0, ESP_ERR_NOT_SUPPORTED, TAG,
                        "reset is driven by the IO expander, not a GPIO");

    uint8_t madctl = 0, colmod = 0, fb_bpp = 0;
    switch (cfg->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        madctl |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported element order");
    }
    switch (cfg->bits_per_pixel) {
    case 16: // RGB565
        colmod = 0x55;
        fb_bpp = 16;
        break;
    case 18: // RGB666, each component in the 6 high bits of a byte
        colmod = 0x66;
        fb_bpp = 24;
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported pixel width");
    }

    st7796_panel_t *st = calloc(1, sizeof(st7796_panel_t));
    ESP_RETURN_ON_FALSE(st, ESP_ERR_NO_MEM, TAG, "no mem for st7796 panel");
    st->io = io;
    st->madctl_val = madctl;
    st->colmod_val = colmod;
    st->fb_bits_per_pixel = fb_bpp;
    st->base.del = panel_st7796_del;
    st->base.reset = panel_st7796_reset;
    st->base.init = panel_st7796_init;
    st->base.draw_bitmap = panel_st7796_draw_bitmap;
    st->base.invert_color = panel_st7796_invert_color;
    st->base.set_gap = panel_st7796_set_gap;
    st->base.mirror = panel_st7796_mirror;
    st->base.swap_xy = panel_st7796_swap_xy;
    st->base.disp_on_off = panel_st7796_disp_on_off;
    *ret_panel = &st->base;
    ESP_LOGD(TAG, "new st7796 panel @%p", st);
    return ESP_OK;
}
