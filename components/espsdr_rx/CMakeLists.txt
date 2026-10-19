# SPDX-License-Identifier: GPL-3.0-or-later
# ESP32-C5 receiver vendored from ESPARGOS esp-sdr (see VENDORED.md).

if(NOT IDF_TARGET STREQUAL "esp32c5")
    message(FATAL_ERROR "espsdr_rx only supports esp32c5, not ${IDF_TARGET}")
endif()

# WHOLE_ARCHIVE keeps chip.h's SOC_RESERVE_MEMORY_REGION entry and the --wrap
# targets in rx_calibration_state.c / rx_recalibration.c linked.
idf_component_register(SRCS "rx_core.c"
                            "upstream/common/rx_recalibration.c"
                            "upstream/common/rx_calibration_state.c"
                       INCLUDE_DIRS "include" "upstream/common"
                       PRIV_INCLUDE_DIRS "upstream/targets/esp32c5"
                       PRIV_REQUIRES esp_event esp_hw_support esp_phy esp_timer esp_wifi heap
                       WHOLE_ARCHIVE)

# burst_gain.h carries upstream's serial GAIN handler, which rx_core.c does not use.
set_source_files_properties(rx_core.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

# Same link options as upstream main/CMakeLists.txt and
# common/rx_calibration_link.cmake for the esp32c5 target.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/upstream/targets/esp32c5/sram_guard.ld"
    "-Wl,--wrap=phy_chip_set_chan_ana"
    "-Wl,--wrap=phy_set_rx_gain_cal_dc")
