# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c" "dsp.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board espsdr_rx esp_timer heap nvs_flash)
