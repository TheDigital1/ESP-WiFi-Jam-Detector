# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c" "audio.c" "bandplan.c" "characterize.c" "detect.c" "detector.c" "dsp.c" "eventlog.c" "link.c" "link_proto.c" "lvgl_port.c" "params.c" "plot.c" "settings.c" "settings_values.c" "sweep.c" "ui.c" "ui_settings.c" "ui_spectrum.c" "ui_status.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board esp_app_format esp_driver_usb_serial_jtag espsdr_rx esp_driver_i2s esp_driver_sdspi esp_timer fatfs heap nvs_flash sdmmc)
