/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Waveshare ESP32-C5-Touch-LCD-3.5 pin and bus map.
 *
 * Every value below is taken from Waveshare's official resource repository,
 * https://github.com/waveshareteam/ESP32-C5-Touch-LCD-3.5 at commit 04e6134
 * (2026-09-30). Paths are relative to that repository:
 *
 *   BSP_H   = example/ESP-IDF-V554/02_lvgl_demo/components/
 *             waveshare__esp32_c5_touch_lcd_3_5/include/bsp/esp32_c5_touch_lcd_3_5.h
 *   BSP_C   = same component, esp32_c5_touch_lcd_3_5.c
 *   ARD_IO  = example/Arduino-v3.3.10/example/07_lvgl_demo/Board_IO.h
 *   README  = README.en.md
 *
 * Do not add or change a pin without citing its source here.
 */
#pragma once

#include "driver/gpio.h"

/* ---- Shared I2C bus (expander, PMIC, touch, RTC, IMU, SHTC3, codec) ---- */
#define BOARD_I2C_PORT        0            /* BSP sdkconfig.defaults: CONFIG_BSP_I2C_NUM=0 */
#define BOARD_I2C_SCL         GPIO_NUM_26  /* BSP_H:79; 08_AXP2101/sdkconfig.defaults:10 */
#define BOARD_I2C_SDA         GPIO_NUM_27  /* BSP_H:80; 08_AXP2101/sdkconfig.defaults:11 */
#define BOARD_I2C_HZ          400000       /* ARD_IO:7; 08_AXP2101/sdkconfig.defaults:9 */

/* ---- CH32V006 IO expander (README:34 calls it CH32V006; BSP uses the ----
 * ---- waveshare/custom_io_expander_ch32v003 1.0.2 driver)              ---- */
#define BOARD_EXIO_ADDR       0x24  /* README:34; ARD_IO:8 */
#define BOARD_EXIO_REG_DIR    0x02  /* custom_io_expander_ch32v003.c: DIRECTION_REG_ADDR; bit=1 -> output */
#define BOARD_EXIO_REG_OUT    0x03  /* custom_io_expander_ch32v003.c: OUTPUT_REG_ADDR */
#define BOARD_EXIO_REG_IN     0x04  /* custom_io_expander_ch32v003.c: INPUT_REG_ADDR */
#define BOARD_EXIO_REG_PWM    0x05  /* custom_io_expander_ch32v003.c: PWM_REG_ADDR (raw 0..255) */
#define BOARD_EXIO_TP_RST     0     /* BSP_H:108 "Connected via IO expander P0"; ARD_IO:12 */
#define BOARD_EXIO_LCD_RST    1     /* BSP_H:106 "Connected via IO expander P1"; ARD_IO:13 */
#define BOARD_EXIO_PA_EN      5     /* 01_factory/components/bsp_extra/src/bsp_board_extra.c:26-30
                                       (Audio_PA_EN); ARD_IO:14 names it BOARD_POWER_EXIO. BSP_C:384
                                       drives it high at init, so we do the same. */
#define BOARD_EXIO_IN6        6     /* BSP_C:379 input; 01_factory/.../app_settings.cpp:48,131 */
/* Backlight: expander PWM register, BSP_H:107 "Connected via IO expander PWM1"; BSP_C:577 */

/* ---- ST7796 LCD on SPI2 (shared with the TF card) ---- */
#define BOARD_LCD_SPI_HOST    SPI2_HOST           /* BSP_H:444 */
#define BOARD_LCD_PCLK_HZ     (60 * 1000 * 1000)  /* BSP_H:443 */
#define BOARD_LCD_SCLK        GPIO_NUM_6          /* BSP_H:103 */
#define BOARD_LCD_MOSI        GPIO_NUM_7          /* BSP_H:102 */
#define BOARD_LCD_DC          GPIO_NUM_5          /* BSP_H:104 */
#define BOARD_LCD_CS          GPIO_NUM_8          /* BSP_H:105 */
#define BOARD_LCD_NATIVE_W    320                 /* BSP include/bsp/display.h:39 */
#define BOARD_LCD_NATIVE_H    480                 /* BSP include/bsp/display.h:40 */
/* Panel init per BSP_C:428-441: BGR element order, colour inversion on,
 * portrait uses mirror_x=1. RGB565 big-endian (display.h:30-34). */

/* ---- TF card (shares SCLK/MOSI with the LCD) ---- */
#define BOARD_SD_CS           GPIO_NUM_9  /* BSP_H:118 */
#define BOARD_SD_MISO         GPIO_NUM_2  /* BSP_H:121 */

/* ---- FT6336 touch ---- */
#define BOARD_TOUCH_ADDR      0x38        /* README:33; esp_lcd_touch_ft6336.h:64 */
#define BOARD_TOUCH_INT       GPIO_NUM_3  /* BSP_H:109 */

/* ---- AXP2101 PMIC ---- */
#define BOARD_PMU_ADDR        0x34  /* README:35; 08_AXP2101/main/main.cpp:52 */
/* PMU IRQ is not wired to a GPIO in Waveshare's demo: 08_AXP2101/sdkconfig.defaults:12 (-1). */

/* ---- Buttons ---- */
#define BOARD_BOOT_BTN        GPIO_NUM_28 /* 01_factory/components/app_camera/camera_ui/camera_ui.c:27;
                                             input + pull-up, active low (same file :112-119, :580) */
/* PWR key goes to the AXP2101 PWRON pin (schematic net PWRON), not to a GPIO. */

/* ---- I2S (ES8311 + NS4150B), for M6 only ---- */
#define BOARD_I2S_BCLK        GPIO_NUM_23 /* BSP_H:87 */
#define BOARD_I2S_MCLK        GPIO_NUM_NC /* BSP_H:88 */
#define BOARD_I2S_WS          GPIO_NUM_10 /* BSP_H:89 */
#define BOARD_I2S_DOUT        GPIO_NUM_25 /* BSP_H:90 */
#define BOARD_I2S_DIN         GPIO_NUM_24 /* BSP_H:91 */
