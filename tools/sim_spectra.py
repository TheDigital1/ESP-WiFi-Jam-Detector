#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic sweep spectra for the host tests of main/detect.c.

Produces, per scenario, the stitched per-sweep spectra the firmware's sweep
task hands the detector: for every grid bin (312.5 kHz) the mean, min and max
power over the K snapshots of its tune, in 0.01 dB. Snapshot times follow the
measured sweep timing (2.4 GHz swept twice per 5 GHz sweep, ~54 ms per
2.4 GHz tune, ~69 ms per 5 GHz tune, snapshots ~5 ms apart), so bursty
signals, the microwave's 50 Hz on/off cycle and a sweeping tone are sampled
the way the receiver samples them.

Nothing here transmits anything; it only writes files.

Usage: sim_spectra.py --out DIR [--only NAME ...]
File format (little endian): b"JDSIM2\\0\\0", u32 records, then per record
  u8 band, u8 0, u16 n_bins, u32 t_start_ms, u32 t_ms, u16 gain, u16 clipped,
  u16 snapshots, u16 0, int16 avg[n_bins], int16 min[n_bins], int16 max[n_bins]
"""
import argparse
import math
import os
import random
import struct
from array import array

RES = 0.3125  # MHz per grid bin
K = 4  # snapshots per tune
SNAP_MS = 5.25
DSP_MS = 13.0
FLOOR_DB = 30.0
CLIP_DB = 84.0  # FFT-domain total power at which a snapshot clips
SCHEDULE = (0, 0, 1)  # 2.4 GHz weighted 2:1, as in app_main.c

BANDS = {
    0: dict(name="2.4", start=2400.0, stop=2483.5, step=48.0, tune_ms=20.0, gain=68),
    1: dict(name="5", start=5150.0, stop=5895.0, step=48.0, tune_ms=35.0, gain=81),
}
for b in BANDS.values():
    b["n"] = math.ceil(round((b["stop"] - b["start"]) / RES, 6))
    b["slices"] = math.ceil((b["stop"] - b["start"]) / b["step"])

T_LEARN_END = 60.0  # detector learns for 60 s (default config)
T_EVENT = (70.0, 130.0)
T_END = 160.0


def db2lin(db):
    return 10.0 ** (db / 10.0)


def bin_of(band, f):
    return int((f - BANDS[band]["start"]) / RES)


def active(t, window):
    return window is None or window[0] <= t < window[1]


class Signal:
    band = 0
    window = None

    def add(self, t, lo, hi, p):
        raise NotImplementedError


def add_tone(band, f, power, lo, hi, p):
    """A tone's Hann main lobe spans about one grid bin; split it between the
    two nearest bins."""
    pos = (f - BANDS[band]["start"]) / RES - 0.5
    k0 = math.floor(pos)
    frac = pos - k0
    for k, w in ((k0, 1.0 - frac), (k0 + 1, frac)):
        if lo <= k < hi:
            p[k - lo] += w * power


class Tone(Signal):
    def __init__(self, band, f, db, window=None):
        self.band, self.f, self.power, self.window = band, f, db2lin(db), window

    def add(self, t, lo, hi, p):
        if active(t, self.window):
            add_tone(self.band, self.f, self.power, lo, hi, p)


class WiFi(Signal):
    """One link: OFDM spectrum (802.11 mask), on/off bursts as a two-state
    Markov process with mean busy time burst_ms and the given airtime."""

    def __init__(self, rng, band, centre, bw, db, airtime, burst_ms, window=None):
        self.rng, self.band, self.window = rng, band, window
        self.busy_ms = burst_ms
        self.idle_ms = burst_ms * (1.0 - airtime) / airtime
        self.busy = rng.random() < airtime
        self.until = 0.0
        half = bw / 2.0
        # (offset from centre in MHz, dBr): flat top, then the spectral mask.
        mask = ((half - 1.0, 0.0), (half + 1.0, -20.0), (bw, -28.0), (1.5 * bw, -40.0))
        self.shape = {}
        for k in range(BANDS[band]["n"]):
            f = BANDS[band]["start"] + (k + 0.5) * RES
            d = abs(f - centre)
            if d >= mask[-1][0]:
                continue
            prev = (0.0, 0.0)
            for edge in mask:
                if d <= edge[0]:
                    x0, y0 = prev
                    x1, y1 = edge
                    rel = y0 + (y1 - y0) * (d - x0) / (x1 - x0) if x1 > x0 else y1
                    break
                prev = edge
            self.shape[k] = db2lin(db + rel)

    def on(self, t):
        ms = t * 1000.0
        while ms >= self.until:
            self.busy = not self.busy
            mean = self.busy_ms if self.busy else self.idle_ms
            self.until += self.rng.expovariate(1.0 / mean)
        return self.busy

    def add(self, t, lo, hi, p):
        if active(t, self.window) and self.on(t):
            for k in range(lo, hi):
                v = self.shape.get(k)
                if v:
                    p[k - lo] += v


class Ble(Signal):
    """Advertising / connection events: a 1 MHz wide hop on a random BLE
    channel, present in a snapshot with probability prob."""

    def __init__(self, rng, db, prob):
        self.rng, self.band, self.power, self.prob = rng, 0, db2lin(db), prob

    def add(self, t, lo, hi, p):
        if self.rng.random() < self.prob:
            f = 2402.0 + 2.0 * self.rng.randrange(40)
            for df in (-0.3125, 0.0, 0.3125):
                add_tone(0, f + df, self.power / 3.0, lo, hi, p)


class Noise(Signal):
    """Continuous noise raising the floor by db over [f_lo, f_hi)."""

    def __init__(self, band, f_lo, f_hi, db, window):
        self.band, self.window = band, window
        self.k_lo, self.k_hi = bin_of(band, f_lo), bin_of(band, f_hi) + 1
        self.power = db2lin(FLOOR_DB) * (db2lin(db) - 1.0)

    def add(self, t, lo, hi, p):
        if active(t, self.window):
            for k in range(max(lo, self.k_lo), min(hi, self.k_hi)):
                p[k - lo] += self.power


class Swept(Signal):
    """Sawtooth sweep from f_lo to f_hi every period_s."""

    def __init__(self, band, f_lo, f_hi, period_s, db, window):
        self.band, self.window = band, window
        self.f_lo, self.span, self.period, self.power = f_lo, f_hi - f_lo, period_s, db2lin(db)

    def add(self, t, lo, hi, p):
        if active(t, self.window):
            frac = ((t - self.window[0]) / self.period) % 1.0
            add_tone(self.band, self.f_lo + frac * self.span, self.power, lo, hi, p)


class Microwave(Signal):
    """Magnetron on during one half of each 50 Hz mains cycle; its carrier
    drifts 2440 -> 2470 MHz over the half cycle (3 MHz wide) above a broad
    noise skirt over the whole band."""

    def __init__(self, peak_db, skirt_db, window):
        self.band, self.window = 0, window
        self.peak, self.skirt = db2lin(peak_db), db2lin(FLOOR_DB) * (db2lin(skirt_db) - 1.0)

    def add(self, t, lo, hi, p):
        if not active(t, self.window):
            return
        cyc = (t * 50.0) % 1.0
        if cyc >= 0.5:
            return
        fc = 2440.0 + 30.0 * cyc * 2.0
        for k in range(lo, hi):
            f = 2400.0 + (k + 0.5) * RES
            v = self.skirt
            d = f - fc
            if abs(d) < 12.0:
                v += self.peak * math.exp(-0.5 * (d / 3.0) ** 2)
            p[k - lo] += v


def background(rng):
    """Always present: the room this was imagined in."""
    return [
        WiFi(rng, 0, 2437.0, 20.0, 55.0, 0.10, 1.0),  # own AP, ch 6
        WiFi(rng, 0, 2412.0, 20.0, 45.0, 0.02, 0.4),  # neighbour beacons, ch 1
        WiFi(rng, 0, 2462.0, 20.0, 42.0, 0.02, 0.4),  # neighbour beacons, ch 11
        Ble(rng, 50.0, 0.10),
        Tone(0, 2440.0, 42.0),  # a fixed spur the baseline must absorb
        WiFi(rng, 1, 5180.0, 20.0, 50.0, 0.05, 0.8),  # AP ch 36
        WiFi(rng, 1, 5530.0, 80.0, 52.0, 0.10, 1.0),  # AP ch 106, 80 MHz
        Tone(1, 5500.0, 40.0),
        Tone(1, 5800.0, 38.0),
    ]


def scenarios():
    ev = T_EVENT
    return {
        "clean": lambda rng: [],
        "wifi_busy": lambda rng: [
            WiFi(rng, 0, 2437.0, 20.0, 62.0, 0.80, 2.5, ev),  # iperf on ch 6
            WiFi(rng, 0, 2412.0, 20.0, 48.0, 0.30, 1.5, ev),
            WiFi(rng, 0, 2462.0, 20.0, 52.0, 0.40, 1.5, ev),
            WiFi(rng, 1, 5530.0, 80.0, 58.0, 0.90, 3.0, ev),  # iperf on the 80 MHz channel
            WiFi(rng, 1, 5180.0, 20.0, 52.0, 0.30, 1.5, ev),
            WiFi(rng, 1, 5745.0, 20.0, 50.0, 0.50, 2.0, ev),
        ],
        "wideband24": lambda rng: [Noise(0, 2400.0, 2483.5, 15.0, ev)],
        "wideband5": lambda rng: [Noise(1, 5150.0, 5895.0, 12.0, ev)],
        "cw": lambda rng: [Tone(0, 2437.3, 55.0, ev), Tone(1, 5612.1, 50.0, ev)],
        "swept": lambda rng: [
            Swept(0, 2400.0, 2483.5, 20.0, 55.0, ev),
            Swept(1, 5150.0, 5895.0, 40.0, 55.0, ev),
        ],
        "microwave": lambda rng: [Microwave(65.0, 16.0, ev)],
        "drift": lambda rng: [],
    }


def drift_db(name, t):
    """'drift': the floor rises 4 dB over the event window and stays (antenna
    moved, hand nearby); the slow baseline must cope without alarming."""
    if name != "drift" or t < T_EVENT[0]:
        return 0.0
    return 4.0 * min(1.0, (t - T_EVENT[0]) / (T_EVENT[1] - T_EVENT[0]))


def floor_db(band, k):
    """Per-bin receiver floor: filter roll-off towards each tune's edges and a
    per-tune gain offset, as seen in M3's characterisation."""
    b = BANDS[band]
    f = b["start"] + (k + 0.5) * RES
    s = min(b["slices"] - 1, int((f - b["start"]) / b["step"]))
    x = (f - (b["start"] + b["step"] * (s + 0.5))) / (b["step"] / 2.0)
    return FLOOR_DB - 1.5 * x * x + 0.3 * math.sin(7.0 * x) + ((s * 37) % 5 - 2) * 0.4


def cdb(p):
    return int(round(1000.0 * math.log10(p))) if p > 0 else 0


def run(name, out_dir, seed):
    rng = random.Random(seed)
    gamma = [rng.gammavariate(4.0, 1.0) / 4.0 for _ in range(1 << 16)]  # mean of 4 FFT bins of noise
    bits = rng.getrandbits
    signals = background(rng) + scenarios()[name](rng)
    floors = {band: [db2lin(floor_db(band, k)) for k in range(b["n"])] for band, b in BANDS.items()}

    records = []
    t = 0.0
    while t < T_END:
        for band in SCHEDULE:
            b = BANDS[band]
            n = b["n"]
            sig = [s for s in signals if s.band == band]
            avg, mn, mx = [0] * n, [0] * n, [0] * n
            clipped = 0
            t_start = t
            for s in range(b["slices"]):
                lo = bin_of(band, b["start"] + s * b["step"]) if s else 0
                hi = n if s == b["slices"] - 1 else bin_of(band, b["start"] + (s + 1) * b["step"])
                t += b["tune_ms"] / 1000.0
                fl = [v * db2lin(drift_db(name, t)) for v in floors[band][lo:hi]]
                acc = [0.0] * (hi - lo)
                lo_p = [math.inf] * (hi - lo)
                hi_p = [0.0] * (hi - lo)
                for _ in range(K):
                    p = [v * gamma[bits(16)] for v in fl]
                    for sg in sig:
                        sg.add(t, lo, hi, p)
                    if 10.0 * math.log10(sum(p)) + 6.0 > CLIP_DB:
                        clipped += 1
                    for i, v in enumerate(p):
                        acc[i] += v
                        if v < lo_p[i]:
                            lo_p[i] = v
                        if v > hi_p[i]:
                            hi_p[i] = v
                    t += SNAP_MS / 1000.0
                t += DSP_MS / 1000.0
                for i in range(hi - lo):
                    avg[lo + i] = cdb(acc[i] / K)
                    mn[lo + i] = cdb(lo_p[i])
                    mx[lo + i] = cdb(hi_p[i])
            records.append(
                struct.pack("<BBHIIHHHH", band, 0, n, int(t_start * 1000.0), int(t * 1000.0), b["gain"], clipped,
                            K * b["slices"], 0)
                + array("h", avg).tobytes()
                + array("h", mn).tobytes()
                + array("h", mx).tobytes()
            )
    path = os.path.join(out_dir, name + ".bin")
    with open(path, "wb") as f:
        f.write(b"JDSIM2\0\0" + struct.pack("<I", len(records)))
        for r in records:
            f.write(r)
    return path, len(records)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--seed", type=int, default=1000, help="base seed (tests use the default)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    names = list(scenarios())
    for i, name in enumerate(names):
        if args.only and name not in args.only:
            continue
        path, n = run(name, args.out, seed=args.seed + i)
        print(f"{path}: {n} sweeps")


if __name__ == "__main__":
    main()
