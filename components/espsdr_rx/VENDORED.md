# Vendored from ESPARGOS esp-sdr

- Upstream: https://github.com/ESPARGOS/esp-sdr
- Commit: `f86da81807b7f6e850742f6b789d9468c190ad41` (2026-10-17)
- Licence: GPL-3.0-or-later (see `/LICENSE`). Copyright ESPARGOS contributors.
- Required ESP-IDF: `25fe69f946311abdaf9ad56591f25fedbc20ac98` (from upstream
  `firmware-targets.json`, profile `esp32c5`). The receiver relies on private
  PHY ABI offsets, so do not change the IDF version.

| File here | Upstream path | Status |
|---|---|---|
| `upstream/targets/esp32c5/sram_guard.ld` | `main/targets/esp32c5/sram_guard.ld` | verbatim |
| `upstream/targets/esp32c5/chip.h` | `main/targets/esp32c5/chip.h` | verbatim (capture bank reservation, `adctrig`) |
| `upstream/targets/esp32c5/tuning.h` | `main/targets/esp32c5/tuning.h` | verbatim |
| `upstream/common/rx_tuning.h` | `main/common/rx_tuning.h` | verbatim |
| `upstream/common/rx_bandwidth.h` | `main/common/rx_bandwidth.h` | verbatim |
| `upstream/common/burst_gain.h`, `burst_gain_min.h` | `main/common/` | verbatim |
| `upstream/common/rx_recalibration.{h,c}` | `main/common/` | verbatim |
| `upstream/common/rx_calibration_state.c` | `main/common/` | verbatim |
| `upstream/common/spectrum_fft.h` | `main/common/spectrum_fft.h` | verbatim (Q15 FFT kernel, used by `main/dsp.c`) |
| `upstream/families/c5_c6_c61/receiver.c.orig` | `main/families/c5_c6_c61/receiver.c` | verbatim reference copy, not compiled |
| `rx_core.c` | `main/families/c5_c6_c61/receiver.c` | **modified**, see header; diff against `receiver.c.orig` |

Link options carried over from upstream `main/CMakeLists.txt` and
`main/common/rx_calibration_link.cmake` (esp32c5): `-T sram_guard.ld`,
`--wrap=phy_chip_set_chan_ana`, `--wrap=phy_set_rx_gain_cal_dc`.
sdkconfig keys carried over from `sdkconfig.defaults.esp32c5` are in the
project's `sdkconfig.defaults`.

Not vendored: the serial protocol (`burst_serial`, `burst_commands.h`,
`burst_capture.h`), on-chip spectrum (`spectrum.c`, whose static buffers
alone took ~27 KB of internal SRAM), GPIO commands, versioning and probes.
