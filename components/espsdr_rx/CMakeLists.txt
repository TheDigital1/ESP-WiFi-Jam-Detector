# SPDX-License-Identifier: GPL-3.0-or-later
# ESP32-C5 receiver vendored from ESPARGOS esp-sdr (see VENDORED.md).
# M1: only the capture-SRAM reservation and linker guard. Receiver code lands in M2.

if(NOT IDF_TARGET STREQUAL "esp32c5")
    message(FATAL_ERROR "espsdr_rx only supports esp32c5, not ${IDF_TARGET}")
endif()

# WHOLE_ARCHIVE: the reservation is only a section entry, nothing references it.
idf_component_register(SRCS "capture_region.c"
                       PRIV_REQUIRES heap
                       WHOLE_ARCHIVE)

# Keep the linker guard beside the backend whose SRAM aperture it protects
# (same placement as upstream main/CMakeLists.txt).
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/upstream/targets/esp32c5/sram_guard.ld")
