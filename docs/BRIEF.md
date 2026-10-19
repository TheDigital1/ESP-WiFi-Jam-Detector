# Project brief: Dual-band (2.4 + 5 GHz) RF jamming detector on ESP32-C5

You are helping me build firmware for a **receive-only** RF interference / jamming
detector. It runs standalone on a Waveshare **ESP32-C5-Touch-LCD-3.5** board and
shows the RF noise floor and spectrum on the built-in touchscreen. The board is
plugged into this machine over USB-C for flashing and serial logs.

Work in milestones (below). After each milestone, stop, tell me exactly what to
flash/check on the device, and wait for my confirmation before moving on.

---

## 1. Hard rules

- **Receive only.** Do not implement, enable, or experiment with any RF transmit
  path, packet injection, deauth frames, or anything that could interfere with
  other devices. Testing must use naturally occurring signals or synthetic data
  (see Section 9).
- **Do not guess board pins.** Get every GPIO (LCD SPI, backlight, touch I2C,
  AXP2101 PMIC, ES8311 audio, buttons, SD card) from Waveshare's official
  wiki/demo code for this exact board. Cite the file you took each pin from in a
  comment in `board_pins.h`.
- The SDR receiver code comes from **ESPARGOS/esp-sdr**, which is GPL-3.0-or-later.
  This project is therefore GPL-3.0-or-later too. Keep their copyright notices.
- Keep esp-sdr's **capture-SRAM linker guards** intact. The capture engine owns a
  fixed SRAM region that the CPU must not use while capturing. Never put LVGL or
  FFT buffers there.
- While SDR capture is active, do not use normal Wi-Fi (scan / connect / AP). Doing
  so retunes the radio. Wi-Fi networking is out of scope for v1.

## 2. Hardware facts (verify against the wiki)

- MCU: ESP32-C5 (RISC-V, up to 240 MHz), 8 MB PSRAM, 32 MB flash, dual-band
  2.4/5 GHz Wi-Fi. External antenna on an IPEX connector.
- Display: 3.5" IPS 320×480, **ST7796** over SPI. Use landscape 480×320 for the
  spectrum.
- Touch: **FT6336** over I2C. The `esp_lcd_touch_ft5x06` driver usually works for
  it; confirm.
- PMIC: **AXP2101** on I2C. It may need initialising before the LCD or backlight
  powers up. Check Waveshare's demo for the exact init sequence.
- Also on board: ES8311 audio codec + NS4150B amp + speaker, QMI8658 IMU,
  PCF85063 RTC, SHTC3, TF card slot, BOOT and PWR buttons.
- References:
  - Product: https://www.waveshare.com/esp32-c5-touch-lcd-3.5.htm
  - Wiki: https://docs.waveshare.com/ESP32-C5-Touch-LCD-3.5
  - Waveshare GitHub: https://github.com/waveshareteam (find this board's ESP-IDF demo)

## 3. SDR capability we're relying on (ESPARGOS esp-sdr)

Repo: https://github.com/ESPARGOS/esp-sdr. Read the README,
`docs/rx-controls.md` and `docs/spectrum.md` first.

What it supports on the **ESP32-C5**:
- Raw I/Q **snapshot** capture from the Wi-Fi receiver (undocumented debug path).
- Sample rates of 4/8/10/20/40/80 MS/s.
- Analog bandwidth of about 11–48 MHz.
- **5 GHz reception**: tuning above 3000 MHz selects the 5 GHz RF path. The
  supported Wi-Fi range is 5150–5895 MHz.
- On-chip snapshot FFT of 256–2048 bins.
- **No continuous capture on C5.** Reception stops between snapshots, so short
  bursts can be missed. Design the detection logic around this.
- Gain is uncalibrated: hardware AGC by default, manual gain index available.
  dB values are relative, not dBm.
- Relevant code: `main/families/c5_c6_c61/` (shared receiver) and
  `main/targets/esp32c5/`.
- `firmware-targets.json` pins the exact ESP-IDF commit for each target. Use
  that ESP-IDF version.

## 4. Architecture

Prefer this layout. Propose changes if the upstream structure makes it awkward.

```
jammer-detector/
  CMakeLists.txt, sdkconfig.defaults, partitions.csv
  components/
    espsdr_rx/        # receiver code vendored from ESPARGOS (C5 path only), minimal API:
                      #   rx_init(), rx_tune(mhz), rx_set_gain_manual(idx)/auto(),
                      #   rx_set_bw(mhz), rx_capture(n, rate_idx, int8_t* iq_out)
    board/            # AXP2101 init, LCD (esp_lcd ST7796), touch, backlight, buttons, buzzer
  main/
    app_main.c
    sweep.c/.h        # band plan + tuning/capture scheduler (own FreeRTOS task)
    dsp.c/.h          # window, FFT (esp-dsp), power in dB, averaging, max-hold, DC notch
    detect.c/.h       # baseline learning + jamming heuristics + alarm state machine
    ui.c/.h           # LVGL screens
    settings.c/.h     # NVS-persisted thresholds/gain
    board_pins.h
  tools/
    sim_spectra.py    # synthetic spectra generator for host tests
  test_host/          # host-compiled unit tests for dsp.c and detect.c
```

- Use **ESP-IDF** (not Arduino), **LVGL v9** via `esp_lvgl_port`, and
  **esp-dsp** for the FFT.
- Put LVGL draw buffers and large DSP arrays in **PSRAM**. Keep the capture
  buffer where esp-sdr requires it.
- Tasks:
  - **sweep/DSP task** (highest app priority): tune → settle → capture → FFT →
    publish a spectrum segment.
  - **detect task**: consumes segments and updates per-band state.
  - **UI task**: LVGL, ~10–15 fps. Use a mutex or queue between tasks. Never
    block the sweep on rendering.

## 5. Band plan and sweep

- Capture at 40 MS/s (or whatever gives the cleanest result; test 20/40/80).
  Use only the central ~75% of each capture, because filter roll-off hurts the
  edges, and notch the DC spike at the center bin.
- Step the center frequency so the usable slices tile each band without gaps.
  - **2.4 GHz:** 2400–2483.5 MHz, roughly 3–4 tunes.
  - **5 GHz:** 5150–5895 MHz, roughly 20–25 tunes. Group by UNII sub-band
    (5150–5250, 5250–5350, 5470–5730, 5735–5895) so the UI can label them.
- Per tune: K snapshots (default 4, configurable), 1024-pt FFT with a Hann
  window. Compute an average-power spectrum **and** a per-snapshot max.
- Measure and log the time for a full 2.4 GHz sweep and a full 5 GHz sweep.
  Default schedule: alternate bands, 2.4 GHz weighted ~2:1. Make it configurable.
- **Gain:** use a **fixed manual gain** while detecting, because AGC would hide a
  raised noise floor. Add a one-time "auto-range" routine that picks the highest
  gain index that doesn't clip on the strongest normal signal (measure clipping
  from the raw int8 I/Q). Store it in NVS.

## 6. Detection logic (`detect.c`)

The device is uncalibrated, so detect **change relative to a learned baseline**,
not absolute levels.

1. **Baseline learning.** On first boot, on a button press, or from Settings,
   learn for 60 s (configurable). For each frequency bin, store the median of
   the per-sweep noise floor. For each slice, store the 20th-percentile power
   (the "floor"). After learning, update the baseline slowly with an EMA, and
   only while the state is NORMAL.
2. **Metrics per slice per sweep:**
   - `floor_db`: 20th percentile of bins.
   - `floor_delta = floor_db − baseline_floor`.
   - `occupancy`: fraction of bins more than 6 dB above the baseline floor.
   - `duty`: over the last N sweeps, the fraction of sweeps where `floor_delta`
     exceeded the threshold.
   - `flatness`: spectral flatness (geometric mean / arithmetic mean).
3. **Heuristics:**
   - **Wideband / barrage:** `floor_delta` ≥ 10 dB (configurable) across ≥ 60%
     of a band's slices, with high duty for ≥ 5 s.
   - **Narrowband CW:** the same bin is ≥ 15 dB above baseline in ≥ 90% of
     snapshots for ≥ 5 s. Normal Wi-Fi/BLE is bursty, so it won't stay that
     constant.
   - **Swept:** a strong peak whose frequency moves monotonically across
     consecutive sweeps and covers most of the band.
   - **Normal Wi-Fi traffic** must not alarm. It is bursty, so duty should be
     moderate and the floor between bursts should return to baseline.
4. **State machine per band:** NORMAL → SUSPECT (criteria met briefly) → ALARM
   (criteria held ≥ T seconds) → back to NORMAL after a hold-off. Log every
   transition to serial with the metrics.
5. Expose all thresholds in a struct, saved in NVS and editable in Settings.

Be honest in the UI. Label results "interference / possible jamming". This
can't prove intent, and microwave ovens will trigger it on 2.4 GHz.

## 7. UI (480×320 landscape, LVGL)

- **Status screen (default):** two large tiles, "2.4 GHz" and "5 GHz". Each shows
  its state colour (green / amber / red), current floor delta in dB, a duty
  bar, and the last alarm time (use the PCF85063 RTC if it's easy, otherwise
  uptime).
- **Spectrum screen:** a band toggle (2.4 / 5) and a line chart with the current
  average, the baseline (dim), and max-hold (thin). X-axis in MHz with Wi-Fi
  channel ticks (2.4: ch 1/6/11; 5: UNII labels). Y-axis in relative dB.
  Include a "reset max-hold" button.
- **Waterfall screen (stretch goal):** scrolling history for the selected band.
  Render with a canvas plus a colormap lookup table, and keep it cheap.
- **Settings screen:** re-learn baseline, auto-range gain, thresholds (sliders),
  sweep weighting, alarm sound on/off, screen brightness.
- **BOOT button:** cycle screens. **Long-press BOOT:** re-learn baseline.
- **Alarm:** red border flash plus an optional beep through ES8311/NS4150B. Do
  the beep in a later milestone; it's optional.
- Show a small header with the sweep rate (sweeps/s per band) and a "baseline
  learning…" indicator.

## 8. Milestones (stop after each)

- **M0 – Toolchain + stock firmware.**
  1. Install the ESP-IDF version pinned for `esp32c5` in esp-sdr's
     `firmware-targets.json`.
  2. Build and flash the **unmodified** esp-sdr C5 firmware.
  3. Have me confirm it works in the browser viewer
     (https://espargos.net/espsdr/app/) on both 2.4 GHz and 5 GHz.
  4. Detect my OS and serial port yourself (`/dev/ttyACM*`, `/dev/cu.usbmodem*`,
     or `COMx`).
- **M1 – Board bring-up.** In a new project, using pins from Waveshare's demo:
  init AXP2101, turn on the backlight, draw a test pattern on the ST7796, read
  FT6336 touches, and read the BOOT button. No radio yet.
- **M2 – Radio in-app.** Vendor the minimal C5 receiver into `espsdr_rx`.
  1. Capture at 2437 MHz and 5500 MHz.
  2. Print `floor_db` and peak bin over serial.
  3. Verify that clipping detection works.
- **M3 – Sweep + DSP.** Implement the full band plan, averaging, DC notch, and
  stitching into per-band spectra. Log sweep timing. Show a simple live line
  chart.
- **M4 – Detection.** Implement baseline learning, metrics, heuristics, and the
  state machine.
  1. Add host unit tests driven by `tools/sim_spectra.py` (clean, Wi-Fi bursty,
     wideband raised floor, CW tone, swept tone, microwave-like) before tuning on
     hardware.
  2. All tests must pass.
- **M5 – Full UI + settings.** Build the status, spectrum, and settings screens,
  persist settings in NVS, and handle BOOT button navigation.
- **M6 – Polish (optional).** Add the alarm beep, waterfall, SD-card CSV logging
  of alarm events, and battery level from the AXP2101.

## 9. Testing without a jammer

Do **not** build or suggest a transmitter for testing. Use:
- The **synthetic spectra** in host tests (M4).
- A **microwave oven** running a cup of water, for 2.4 GHz wideband interference.
  This should trigger SUSPECT/ALARM on 2.4 GHz only.
- **Heavy Wi-Fi traffic** (e.g. an iperf3 run on a 5 GHz network). This should
  *not* alarm, so use it to tune false-positive thresholds.
- **Moving the antenna / covering it with a hand**. This checks floor stability
  and the slow baseline EMA.

## 10. Coding standards

- C (C17), ESP-IDF style. Use `ESP_LOGI/W/E` with per-module tags and handle
  every `esp_err_t`.
- No dynamic allocation in the sweep loop. Allocate once at init.
- Keep `dsp.c` and `detect.c` free of ESP-IDF headers so they compile on the
  host for unit tests.
- Write a `README.md` covering: build/flash steps, pinned IDF version, the band
  plan, thresholds, known limits (snapshot gaps, uncalibrated dB, 5 GHz sweep
  time, the radio being unavailable for Wi-Fi while scanning), and the GPL
  notice.
- Commit to git after each milestone with a clear message.

## 11. Before writing code

Read the esp-sdr README and docs, the C5 receiver sources, and the Waveshare
demo for this board. Then give me a short plan containing:
- the exact IDF commit,
- the pin table with sources,
- how you'll vendor the receiver,
- any risks you see (e.g. AXP2101 init, SRAM guard conflicts with LVGL/PSRAM,
  5 GHz tuning behaviour).

Then start M0.
