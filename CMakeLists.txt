# SPDX-License-Identifier: GPL-3.0-or-later
# Build with the ESP-IDF commit pinned by esp-sdr: 25fe69f946311abdaf9ad56591f25fedbc20ac98.
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-wifi-jammer-detector)
