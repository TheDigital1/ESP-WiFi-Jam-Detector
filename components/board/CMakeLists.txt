# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "board.c" "st7796.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_lcd
                       PRIV_REQUIRES esp_driver_i2c esp_driver_spi)
