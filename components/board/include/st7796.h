/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from espressif/esp_lcd_st7796 1.0.0; see st7796.c.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_types.h"

/* Create an ST7796 panel on an existing panel IO. reset_gpio_num must be -1:
 * on this board the reset line is on the IO expander. */
esp_err_t st7796_new_panel(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg,
                           esp_lcd_panel_handle_t *ret_panel);
