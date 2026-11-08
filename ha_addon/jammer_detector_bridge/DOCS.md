# Jammer Detector Bridge

This app reads an esp-wifi-jammer-detector over USB and publishes it to your
MQTT broker. Home Assistant's MQTT integration then adds it as a device
automatically, without any YAML.

## Before you start

- **MQTT:** the Mosquitto broker app (or another broker) is running, and the
  MQTT integration is set up in Home Assistant. The app picks up the broker
  login by itself.
- **Firmware:** the detector runs firmware **1.1.0 or later**. Check the
  version on the device's Settings screen or in the release you flashed.
- **Connection:** the detector is plugged into a USB port of the Home
  Assistant machine.

## Install

1. **Settings → Apps → App store**, menu (⋮) → **Repositories**, add
   `https://github.com/TheDigital1/ESP-WiFi-Jam-Detector`.
2. Install **Jammer Detector Bridge**. Home Assistant builds it on your
   machine, which takes a minute or two the first time.
3. On the **Configuration** tab, pick the detector under **Detector**. It is
   listed as `Espressif USB JTAG serial debug unit`. Save.
4. Start the app. Within a few seconds **Settings → Devices & services →
   MQTT** shows a **Jammer Detector** device.

## What you get

Each band (2.4 GHz and 5 GHz) has these entities:

| Entity | Notes |
|---|---|
| *band* alarm | binary sensor, on while the band is in ALARM. Use this for automations. |
| *band* state | learning / normal / suspect / alarm / off (band not scanned) |
| *band* interference | wideband / swept / narrowband / pulsed / none |
| *band* floor rise | noise floor above the learned baseline, dB |
| *band* occupancy | share of the band above its baseline, % |
| *band* alarms since boot | count |
| *band* detector | event entity: one event per state change (`alarm`, `suspect`, `normal`, `learning`) with the band, kind and all detector metrics as attributes |
| *band* clipping, learning | diagnostic |

Device-wide entities:

- **Battery** and **Charging** (diagnostic).
- **Uptime** (diagnostic, disabled by default).

The controls change the detector exactly like its own Settings screen:

- **Re-learn baseline** (button)
- **Alarm beep** (switch)
- **Bands scanned** (select: 2.4 GHz / 5 GHz / Both)
- One number for each detection and timing threshold (wideband rise, narrowband
  rise, swept rise, alarm duty, alarm after, clear after, learn time, and so on)

A change is applied and saved on the device at once, and its screen shows the
new value.

If the app stops or the detector is unplugged, every entity shows as
*unavailable*. The bridge reconnects by itself when the detector comes back.

## Example automation

```yaml
alias: Wi-Fi jamming alert
triggers:
  - trigger: state
    entity_id:
      - binary_sensor.jammer_detector_2_4_ghz_alarm
      - binary_sensor.jammer_detector_5_ghz_alarm
    to: "on"
actions:
  - action: notify.notify
    data:
      title: Possible Wi-Fi jamming
      message: >-
        {{ trigger.to_state.name }}:
        {{ states('sensor.jammer_detector_2_4_ghz_interference') }} /
        {{ states('sensor.jammer_detector_5_ghz_interference') }}
```

Entity IDs depend on your device name. Check them on the device page.

## Options

| Option | |
|---|---|
| Detector | the detector's serial port (a `/dev/serial/by-id/...` path stays the same across reboots) |
| Log level | `debug` also writes the detector's console log to the app log |

## Troubleshooting

- **The device does not appear:** look at the app's **Log** tab.
  - `cannot open`: wrong port, or the detector is unplugged.
  - `no message from the device`: firmware older than 1.1.0, or a different
    device is on that port.
- **The detector restarts when the app starts:** the app opens the port with
  DTR and RTS held high so this does not happen. If it does, please report it
  with the log.
- **Readings look stale after a while:** the detector only streams while the
  bridge pings it (every 15 s). Check the app log for serial errors.

## MQTT topics

Topics are under `jammer/<id>/`, where `<id>` is the detector's MAC address:

- `status`: retained JSON
- `event/24`, `event/5`: events
- `availability`
- `cmd/...`: commands

Discovery is published on `homeassistant/device/jammer_detector_<id>/config`.
The device protocol is described in `docs/LINK.md` in the repository.
