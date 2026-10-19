# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES board espsdr_rx esp_psram esp_timer heap)
