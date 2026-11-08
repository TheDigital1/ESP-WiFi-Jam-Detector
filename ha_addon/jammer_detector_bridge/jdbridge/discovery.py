# SPDX-License-Identifier: GPL-3.0-or-later
"""Home Assistant MQTT device discovery for one detector, built from its hello.

One retained message on <prefix>/device/<object_id>/config describes every
entity. All state comes from the retained status topic, events from the event
topic of each band; commands go to <base>/<id>/cmd/...
"""

from __future__ import annotations

from dataclasses import dataclass

from . import __version__
from .protocol import SCAN_OPTIONS

STATES = ["learning", "normal", "suspect", "alarm", "off"]
KINDS = ["none", "wideband", "swept", "narrowband", "pulsed"]
EVENT_TYPES = ["learning", "normal", "suspect", "alarm"]
REPO_URL = "https://github.com/TheDigital1/ESP-WiFi-Jam-Detector"


@dataclass(frozen=True)
class Topics:
    base: str
    device_id: str
    prefix: str = "homeassistant"

    @property
    def root(self) -> str:
        return f"{self.base}/{self.device_id}"

    @property
    def status(self) -> str:
        return f"{self.root}/status"

    @property
    def availability(self) -> str:
        return f"{self.root}/availability"

    @property
    def bridge_availability(self) -> str:
        return f"{self.base}/bridge/availability"

    def event(self, band_slug: str) -> str:
        return f"{self.root}/event/{band_slug}"

    def cmd(self, *parts: str) -> str:
        return "/".join([self.root, "cmd", *parts])

    @property
    def cmd_wildcard(self) -> str:
        return f"{self.root}/cmd/#"

    @property
    def discovery(self) -> str:
        return f"{self.prefix}/device/jammer_detector_{self.device_id}/config"


def band_slug(band: str) -> str:
    """'2.4' -> '24', '5' -> '5' (entity and topic ids)."""
    return band.replace(".", "")


def _band(band: str) -> str:
    """Template expression for one band's status object."""
    return f"value_json.bands['{band}']"


def build(hello: dict, topics: Topics) -> dict:
    """The device discovery payload."""
    dev_id = topics.device_id
    mac = ":".join(dev_id[i : i + 2] for i in range(0, len(dev_id), 2))
    cmps: dict[str, dict] = {}

    def add(key: str, platform: str, name: str, **opts) -> None:
        cmps[key] = {"p": platform, "name": name, "uniq_id": f"jammer_detector_{dev_id}_{key}", **opts}

    for band in hello.get("bands", []):
        s = band_slug(band)
        b = _band(band)
        label = f"{band} GHz"
        state = topics.status
        add(f"alarm_{s}", "binary_sensor", f"{label} alarm", stat_t=state, dev_cla="problem",
            val_tpl=f"{{{{ 'ON' if {b}.scan and {b}.state == 'alarm' else 'OFF' }}}}")
        add(f"state_{s}", "sensor", f"{label} state", stat_t=state, dev_cla="enum", options=STATES,
            val_tpl=f"{{{{ {b}.state if {b}.scan else 'off' }}}}", ic="mdi:access-point-network")
        add(f"kind_{s}", "sensor", f"{label} interference", stat_t=state, dev_cla="enum", options=KINDS,
            val_tpl=f"{{{{ {b}.kind if {b}.scan else 'none' }}}}", ic="mdi:sine-wave")
        add(f"floor_{s}", "sensor", f"{label} floor rise", stat_t=state, unit_of_meas="dB", stat_cla="measurement",
            sug_dsp_prc=1, val_tpl=f"{{{{ {b}.floor_db if {b}.scan and {b}.state != 'learning' else None }}}}",
            ic="mdi:chart-bell-curve")
        add(f"occupancy_{s}", "sensor", f"{label} occupancy", stat_t=state, unit_of_meas="%",
            stat_cla="measurement", val_tpl=f"{{{{ {b}.occupancy_pct if {b}.scan else None }}}}", ic="mdi:chart-bar")
        add(f"alarms_{s}", "sensor", f"{label} alarms since boot", stat_t=state, stat_cla="total_increasing",
            val_tpl=f"{{{{ {b}.alarms }}}}", ic="mdi:alarm-light")
        add(f"clipped_{s}", "sensor", f"{label} clipping", stat_t=state, unit_of_meas="%", stat_cla="measurement",
            ent_cat="diagnostic", val_tpl=f"{{{{ {b}.clipped_pct if {b}.scan else None }}}}")
        add(f"learn_{s}", "sensor", f"{label} learning", stat_t=state, unit_of_meas="%", ent_cat="diagnostic",
            val_tpl=f"{{{{ {b}.learn_pct }}}}", ic="mdi:school")
        add(f"event_{s}", "event", f"{label} detector", stat_t=topics.event(s), evt_typ=EVENT_TYPES,
            ic="mdi:alert-circle-outline")

    add("battery", "sensor", "Battery", stat_t=topics.status, dev_cla="battery", unit_of_meas="%",
        stat_cla="measurement", ent_cat="diagnostic",
        val_tpl="{{ value_json.power.pct if value_json.power is defined and value_json.power.pct is defined "
        "else None }}")
    add("charging", "binary_sensor", "Charging", stat_t=topics.status, dev_cla="battery_charging",
        ent_cat="diagnostic",
        val_tpl="{{ 'ON' if value_json.power is defined and value_json.power.charging else 'OFF' }}")
    add("uptime", "sensor", "Uptime", stat_t=topics.status, dev_cla="duration", unit_of_meas="s",
        ent_cat="diagnostic", en=False, val_tpl="{{ value_json.up }}")

    add("relearn", "button", "Re-learn baseline", cmd_t=topics.cmd("relearn"), ent_cat="config",
        ic="mdi:refresh")
    add("beep", "switch", "Alarm beep", stat_t=topics.status, cmd_t=topics.cmd("set", "beep"), ent_cat="config",
        val_tpl="{{ 'ON' if value_json.set.beep else 'OFF' }}", ic="mdi:volume-high")
    scan_map = ", ".join(f"{v}: '{k}'" for k, v in SCAN_OPTIONS.items())
    add("scan", "select", "Bands scanned", stat_t=topics.status, cmd_t=topics.cmd("set", "scan"), ent_cat="config",
        options=list(SCAN_OPTIONS), val_tpl=f"{{{{ {{{scan_map}}}[value_json.set.scan] }}}}", ic="mdi:radar")
    for p in hello.get("params", []):
        k = p["k"]
        opts = {"unit_of_meas": p["u"]} if p.get("u") else {}
        add(f"set_{k}", "number", p["n"], stat_t=topics.status, cmd_t=topics.cmd("set", k), ent_cat="config",
            min=p["min"], max=p["max"], step=p["step"], mode="box", val_tpl=f"{{{{ value_json.set.{k} }}}}",
            **opts)

    return {
        "dev": {
            "ids": [f"jammer_detector_{dev_id}"],
            "name": "Jammer Detector",
            "mf": "esp-wifi-jammer-detector",
            "mdl": hello.get("model", ""),
            "sw": hello.get("fw", ""),
            "sn": dev_id,
            "cns": [["mac", mac]],
        },
        "o": {"name": "jammer-detector-bridge", "sw": __version__, "url": REPO_URL},
        "avty": [{"t": topics.bridge_availability}, {"t": topics.availability}],
        "avty_mode": "all",
        "qos": 1,
        "cmps": cmps,
    }


def event_payload(msg: dict) -> dict:
    """An event entity message from a device event: event_type is the new state."""
    out = {k: v for k, v in msg.items() if k not in ("t", "v")}
    out["event_type"] = msg.get("to", "")
    return out
