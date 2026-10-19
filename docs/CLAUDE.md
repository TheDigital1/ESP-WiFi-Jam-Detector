# esp-wifi-jammer-detector (ESP32-C5-Touch-LCD-3.5)
Full spec: docs/BRIEF.md. Read the relevant section before each milestone.

## Hard rules
- Receive only. No RF transmit, injection or deauth code, ever.
- Board pins only from Waveshare's official demo/wiki; cite sources in board_pins.h.
- Keep esp-sdr's capture-SRAM linker guards; LVGL/DSP buffers go in PSRAM.
- Stop after each milestone and tell me what to flash and check.

## Toolchain
- ESP-IDF 25fe69f9 (6.2-dev, pinned by esp-sdr firmware-targets.json) in ~/esp/esp-idf-espsdr.
  `. ~/esp/esp-idf-espsdr/export.sh`. Don't use ~/esp-idf (v5.4.2, other projects).
- esp-sdr reference checkout: ~/esp/esp-sdr @ f86da81. Board on /dev/ttyACM0.

## Status
- M0 done (stock esp-sdr verified in browser viewer on 2.4 and 5 GHz).
- Current milestone: M1
