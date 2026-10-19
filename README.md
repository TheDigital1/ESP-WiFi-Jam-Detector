# esp-wifi-jammer-detector

A **receive-only** 2.4 GHz + 5 GHz RF interference / possible-jamming detector
for the Waveshare **ESP32-C5-Touch-LCD-3.5**. It uses the ESP32-C5's own Wi-Fi
receiver as a snapshot SDR (via [ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr))
and shows the noise floor and spectrum on the built-in touchscreen.

It never transmits, injects or deauthenticates anything. It can show that the
RF environment changed; it cannot prove intent. A microwave oven will look
like interference on 2.4 GHz.

Status: milestones M0–M3 done (board bring-up, receiver, full sweep and live
spectrum). Detection (M4) and the full UI (M5) are next.

## Build and flash

The receiver depends on private PHY internals of one exact ESP-IDF commit, so
use that commit and nothing else:

| | |
|---|---|
| ESP-IDF | `25fe69f946311abdaf9ad56591f25fedbc20ac98` (6.2-dev), from esp-sdr's `firmware-targets.json` (`esp32c5`) |
| esp-sdr | `f86da81807b7f6e850742f6b789d9468c190ad41`, vendored in `components/espsdr_rx` |
| LVGL | 9.5.0 (component manager) |

```sh
mkdir -p ~/esp/esp-idf-espsdr && cd ~/esp/esp-idf-espsdr
git init && git remote add origin https://github.com/espressif/esp-idf.git
git fetch --depth 1 origin 25fe69f946311abdaf9ad56591f25fedbc20ac98 && git checkout FETCH_HEAD
git submodule update --init --recursive --depth 1
./install.sh esp32c5

. ~/esp/esp-idf-espsdr/export.sh
cd esp-wifi-jammer-detector
idf.py set-target esp32c5      # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

On Linux, ModemManager may grab the board's USB port and reset it. Stop it, or
add a udev rule with `ENV{ID_MM_DEVICE_IGNORE}="1"` for USB vendor `303a`.

Host unit tests (no board needed): `make -C test_host`.

## Using it

- The spectrum screen shows the current average (cyan, filled) and max-hold
  (orange) in relative dB. The header shows sweeps per second for each band.
- **BOOT** switches between 2.4 GHz and 5 GHz. **Reset max** clears max-hold.
- At power-up the receiver auto-ranges its gain for a few seconds (see below).

## How it works

### Receiver

Snapshot capture from the C5 Wi-Fi receiver through esp-sdr's debug path:
up to 16,380 complex 10-bit samples per snapshot, with reception stopped
between snapshots. Tuning above 3000 MHz selects the 5 GHz RF path.

### Band plan (measured in M3)

Captures run at **80 MS/s with the widest analog filter**, and the central
**48 MHz** of each 1024-point FFT is kept. That span was the flattest option
measured (within about ±1.5 dB, and the filter is −13 dB at Nyquist, so no
aliasing); 40 MS/s left only about ±15 MHz flat and needed 25 tunes for 5 GHz
instead of 16.

| Band | Range | Tunes (centres) | Grid |
|---|---|---|---|
| 2.4 GHz | 2400–2483.5 MHz | 2 (2424, 2472 MHz) | 268 bins × 312.5 kHz |
| 5 GHz | 5150–5895 MHz | 16 (5174 … 5894 MHz, 48 MHz steps) | 2384 bins × 312.5 kHz |

5 GHz sub-bands are labelled U-NII-1 (5150–5250), U-NII-2A (5250–5350),
U-NII-2C (5470–5730) and U-NII-3 (5735–5895); 5350–5470 MHz is swept but
not labelled.

Per tune: 4 snapshots (configurable), Hann window, Q15 fixed-point FFT (the C5
has no FPU), average power and per-snapshot max, DC spike notched (±2 bins),
then 4 FFT bins are reduced to one 312.5 kHz grid bin and stitched into the
band spectrum. The schedule sweeps 2.4 GHz twice for every 5 GHz sweep.

### Gain

Gain is fixed per band while sweeping: hardware AGC would hide a raised noise
floor. At start-up an auto-range steps down from the top gain index until
clipping across the whole band is at most 100 ppm of samples, then backs off
2 more indices. Typical result here: 2.4 GHz ≈ 65–70, 5 GHz ≈ 81.

### Measured timing (M3, this board)

| | |
|---|---|
| Full 2.4 GHz sweep | ~0.12 s |
| Full 5 GHz sweep | ~1.3 s |
| Per tune | retune + DC recalibration ~25–30 ms at 5 GHz (~7 ms at 2.4 GHz), ~2–5 ms per snapshot, ~2 ms FFT per snapshot |
| Spectrum display | ~10 frames/s |

### Self-interference and the radio-quiet window

LCD SPI traffic measurably affects the 2.4 GHz receiver: with the display
redrawing during captures the floor rose up to ~3 dB at 2462 MHz (no effect at
5 GHz). The firmware therefore never lets an LCD DMA transfer overlap a
capture (`board_rf_quiet_begin/end`). Captures take precedence; a capture
waits at most for the strip already in flight.

### Tasks and CPU (single core)

| Task | Priority | Notes |
|---|---|---|
| LVGL / UI | 11 | short bursts, mostly waiting on LCD DMA; the plot is drawn straight to the panel in strips, not through LVGL |
| sweep | 10 (12 during each ~2 ms capture) | CPU-bound; yields 10 % for detect and idle |

The brief asked for the sweep to have the highest priority. Below the sweep,
the UI only managed about 1 frame/s, because the 25 ms 5 GHz retunes starved
it. With the UI on top it still takes only the CPU it really uses (about 25 %).

## Known limits

- **Snapshot gaps:** each snapshot lasts 12.8 µs (1024 samples at 80 MS/s),
  taken a few milliseconds apart. Short or low-duty-cycle bursts can be missed.
  Detection must rely on statistics over many sweeps.
- **Uncalibrated dB:** all levels are relative (gain-table indices are not
  dB), so detection compares against a learned baseline, not absolute dBm.
- **5 GHz sweep time** is about 1.3 s, dominated by per-frequency DC
  recalibration in the receiver.
- **No Wi-Fi networking:** while sweeping, the radio is retuned constantly, so
  normal Wi-Fi (scan/connect/AP) is not available.
- Normal Wi-Fi start-up runs the PHY's own calibration once at boot, as on every
  ESP32. Retunes use esp-sdr's receive-only DC recalibration with no TX
  loopback.

## Source layout

```
components/board/      Waveshare board support (pins cited in include/board_pins.h)
components/espsdr_rx/  ESP32-C5 receiver vendored from esp-sdr (see VENDORED.md)
main/bandplan.c        tune centres and frequency grid (host-tested)
main/dsp.c             window, FFT, dB, percentiles, DC notch (host-tested)
main/plot.c            spectrum renderer (host-tested)
main/sweep.c           sweep task, auto-range, stitched spectra
main/lvgl_port.c       LVGL display/touch glue, PSRAM allocator
main/ui.c              screens
main/characterize.c    optional receiver/LCD measurements (CONFIG_JD_CHARACTERIZE)
test_host/             host unit tests
```

## Licence

GPL-3.0-or-later (see `LICENSE`), because it includes code from ESPARGOS
esp-sdr (GPL-3.0-or-later, Copyright ESPARGOS contributors). Third-party
notices are in `NOTICE`.
