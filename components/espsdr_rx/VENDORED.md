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
| `capture_region.c` | `SOC_RESERVE_MEMORY_REGION` line of `main/targets/esp32c5/chip.h` | extracted, unchanged |

The receiver sources (`families/c5_c6_c61/receiver.c`, `common/rx_*`) arrive in M2.
