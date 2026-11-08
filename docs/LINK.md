# USB link protocol (version 1)

The detector talks to a host (the Home Assistant bridge, or anything else) over
the board's USB-C port: the ESP32-C5's built-in USB Serial/JTAG, which shows up
as `/dev/ttyACM*` (use `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`).
The same port carries the console log. Firmware: `main/link_proto.c` (format),
`main/link.c` (tasks), `main/params.c` (settings table).

## Opening the port without resetting the board

The USB Serial/JTAG resets the chip when the host sets **DTR low with RTS
high** (how `idf.py` / esptool reset it). Linux raises both lines when a port
is opened; pyserial's default then lowers DTR before RTS, which resets the
board. Keep both high:

```python
s = serial.Serial()
s.port = "/dev/ttyACM0"
s.dtr = True
s.rts = True
s.open()
```

Checked on the board: reopening this way 3 times kept the uptime counting
(no `rst:` line). Closing the port does not reset it either. Baud rate is ignored.

## Framing

- One JSON object per line, UTF-8, ending `\r\n` (the console adds the `\r`).
- Device messages start with `{"t":`. Every other line is console log
  (`I (1234) tag: ...`) and should be ignored. To be robust against a log line
  cut short by a reset, look for `{"t":` anywhere in the line and parse from
  there; drop lines that do not parse.
- Every device message has `"v"` (protocol version). A host should ignore
  fields it does not know and warn if `v` is higher than it supports.
- **Streaming needs a host.** The device sends `status` and `event` only
  while a host has sent a command in the last 60 s. Send `ping` every 15 s or
  so. With no host, nothing is written (so nothing piles up and no USB traffic
  is made).
- **Old lines come first.** The USB driver keeps up to 2 KB that was written
  while no program read the port, and it keeps the *oldest* lines. They arrive
  first after the port is opened and can be minutes old. A host should ignore
  everything until the answer to its first command: send
  `{"c":"ping","id":<random>}` and drop all messages until the `ack` with that
  id. The bridge does this.
- If the host stops reading, the device drops messages; it never waits.

## Device → host

### `hello`

Sent in answer to the `hello` command (then a `status`). `id` is
the chip's base MAC (stable, unique). `params` describes every adjustable
setting, so a host can build its controls from it.

```json
{"t":"hello","v":1,"id":"10bda3fefff2","fw":"1.1.0","model":"ESP32-C5-Touch-LCD-3.5","bands":["2.4","5"],
 "params":[{"k":"learn_s","n":"Learn time","u":"s","min":10,"max":300,"step":10},
           {"k":"wide_db","n":"Wideband rise","u":"dB","min":3,"max":30,"step":1}, ...]}
```

### `status`

While a host is active: every 10 s, after every state change and after every
command (at most two per second).

```json
{"t":"status","v":1,"up":3600,"boot":29,
 "bands":{"2.4":{"scan":true,"state":"alarm","kind":"pulsed","learn_pct":100,"alarms":3,
                 "last_alarm_kind":"pulsed","floor_db":12.4,"occupancy_pct":41,"clipped_pct":0,"duty_pct":90},
          "5":{"scan":false,"state":"normal","kind":"none", ...}},
 "power":{"vbus":true,"battery":true,"charging":false,"pct":85,"mv":4012},
 "set":{"beep":false,"scan":1,"learn_s":60,"wide_db":10,"wide_pct":60,"cw_db":15,"cw_pct":90,"swept_db":15,
        "swept_pct":60,"duty_pct":80,"hold_s":5,"clear_s":10,"weight":6,"brightness":70}}
```

| Field | Meaning |
|---|---|
| `up` | seconds since boot |
| `boot` | boot counter (matches the TF-card log's `boot` column) |
| `bands.<b>.scan` | band is being swept (a band that is off never alarms) |
| `state` | `learning`, `normal`, `suspect`, `alarm` |
| `kind` | reason for `suspect` / `alarm`: `wideband`, `swept`, `narrowband`, `pulsed`; `none` otherwise |
| `learn_pct` | baseline learning progress |
| `alarms`, `last_alarm_kind` | alarms since boot and the kind of the latest |
| `floor_db` | median noise-floor rise over the learned baseline, dB |
| `occupancy_pct`, `clipped_pct`, `duty_pct` | latest sweep's metrics (as on the status screen) |
| `power` | absent if the power chip could not be read; `pct`/`mv` absent without a battery |
| `set` | current settings: `beep`, `scan` (bit 0 = 2.4 GHz, bit 1 = 5 GHz) and every `params` key |

### `event`

One per detector state change while a host is active, with the same fields as a row of the TF-card
CSV log (`up` is uptime in seconds, other names as the CSV header).

```json
{"t":"event","v":1,"boot":29,"up":19.7,"band":"2.4","from":"normal","to":"suspect","kind":"swept",
 "floor_db":-0.28,"continuous_db":-0.22,"max_cell_db":0.05,"max_cell_mhz":2455.0, ...}
```

### `ack`

Answer to every command except `hello`. `id` echoes the command's `id` (0 if
it had none, or if the line could not be parsed).

```json
{"t":"ack","v":1,"id":11,"ok":true}
{"t":"ack","v":1,"id":12,"ok":false,"err":"bad key or value"}
```

Errors: `parse`, `too long` (over 255 bytes), `unknown command`,
`missing k or v`, `bad key or value`, `busy`, `failed`.

## Host → device

Flat JSON objects, one per line (`\n` or `\r\n`). String, integer and
`true`/`false` values only; no escapes, nesting or floats. `id` is optional
(integers up to 1000000). Any valid command makes the host active for 60 s.

| Command | Effect |
|---|---|
| `{"c":"ping","id":1}` | `ack` only; keeps the host active |
| `{"c":"hello"}` | answers with `hello` (then a `status`) |
| `{"c":"get","id":1}` | `ack`, then a `status` |
| `{"c":"relearn","id":2}` | forgets both baselines and learns again (as a long BOOT press) |
| `{"c":"set","k":"beep","v":true,"id":3}` | alarm beep on/off |
| `{"c":"set","k":"scan","v":3,"id":4}` | bands scanned: 1 = 2.4 GHz, 2 = 5 GHz, 3 = both |
| `{"c":"set","k":"wide_db","v":8,"id":5}` | any `params` key; the value must be within `min`..`max` and on a `step` |

A `set` works exactly like moving the control on the device: it is applied at
once, saved to flash, and the settings screen shows the new value. Edits still
pending on the screen are saved first, so neither side's change is lost.

## Radio side

Device messages go out inside the sweep's radio-quiet window, like LCD and
TF-card transfers, so USB traffic does not overlap a capture. Console log
lines are not gated; the periodic per-sweep detector lines are now at debug
level, so the default log is mostly state changes and the 10 s sweep summary.
