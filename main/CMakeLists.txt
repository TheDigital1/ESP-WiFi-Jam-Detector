# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c" "bandplan.c" "characterize.c" "detect.c" "detector.c" "dsp.c" "lvgl_port.c" "plot.c" "settings.c" "sweep.c" "ui.c" "ui_settings.c" "ui_spectrum.c" "ui_status.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board espsdr_rx esp_timer heap nvs_flash)
