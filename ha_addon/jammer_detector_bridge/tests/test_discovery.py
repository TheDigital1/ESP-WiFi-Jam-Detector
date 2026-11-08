# SPDX-License-Identifier: GPL-3.0-or-later
"""The discovery payload, with every value template rendered against real status messages."""
import json
import re

import jinja2
import pytest

from jdbridge import discovery
from jdbridge.protocol import SCAN_OPTIONS

TOPICS = discovery.Topics("jammer", "10bda3fefff2")


@pytest.fixture
def payload(hello):
    return discovery.build(hello, TOPICS)


def render(tpl: str, msg: dict) -> str:
    """Render as Home Assistant does for value_json (plain Jinja is enough for these templates)."""
    return jinja2.Environment(undefined=jinja2.ChainableUndefined).from_string(tpl).render(value_json=msg)


def statuses(samples, capture):
    from jdbridge.protocol import parse_line

    real = [m for m in map(parse_line, capture.splitlines()) if m and m["t"] == "status"]
    return real + [m for m in samples if m["t"] == "status"]


def test_device_and_ids(payload, hello):
    dev = payload["dev"]
    assert dev["ids"] == ["jammer_detector_10bda3fefff2"]
    assert dev["sw"] == hello["fw"] and dev["cns"] == [["mac", "10:bd:a3:fe:ff:f2"]]
    assert payload["o"]["name"] and payload["avty_mode"] == "all"
    ids = [c["uniq_id"] for c in payload["cmps"].values()]
    assert len(ids) == len(set(ids))
    assert all(re.fullmatch(r"[a-zA-Z0-9_-]+", k) for k in payload["cmps"])
    assert TOPICS.discovery == "homeassistant/device/jammer_detector_10bda3fefff2/config"
    json.dumps(payload)  # serialisable


def test_entities_present(payload, hello):
    cmps = payload["cmps"]
    for b in ("24", "5"):
        for k in ("alarm", "state", "kind", "floor", "occupancy", "alarms", "clipped", "learn", "event"):
            assert f"{k}_{b}" in cmps
    for k in ("battery", "charging", "uptime", "relearn", "beep", "scan"):
        assert k in cmps
    for p in hello["params"]:
        n = cmps[f"set_{p['k']}"]
        assert n["p"] == "number" and (n["min"], n["max"], n["step"]) == (p["min"], p["max"], p["step"])
        assert n["cmd_t"] == f"jammer/10bda3fefff2/cmd/set/{p['k']}"


def test_templates_render(payload, samples, capture):
    sts = statuses(samples, capture)
    assert len(sts) >= 5
    for key, c in payload["cmps"].items():
        if "val_tpl" not in c:
            continue
        for st in sts:
            out = render(c["val_tpl"], st).strip()
            assert "Undefined" not in out, (key, out)
            if c["p"] == "binary_sensor" or c["p"] == "switch":
                assert out in ("ON", "OFF"), (key, out)
            elif c.get("dev_cla") == "enum":
                assert out in c["options"], (key, out)
            elif c["p"] == "select":
                assert out in c["options"], (key, out)
            elif c["p"] in ("sensor", "number"):
                assert out == "None" or float(out) == float(out), (key, out)


def test_template_values(payload, samples):
    st = next(m for m in samples if m["t"] == "status" and "pct" in m.get("power", {}))
    cmps = payload["cmps"]
    assert render(cmps["alarm_24"]["val_tpl"], st) == "ON"
    assert render(cmps["kind_24"]["val_tpl"], st) == "pulsed"
    assert render(cmps["state_5"]["val_tpl"], st) == "learning"
    assert render(cmps["floor_5"]["val_tpl"], st) == "None"  # no floor while learning
    assert render(cmps["battery"]["val_tpl"], st) == "85"
    assert render(cmps["scan"]["val_tpl"], st) == "Both"
    no_batt = next(m for m in samples if m["t"] == "status" and "power" in m and "pct" not in m["power"])
    assert render(cmps["battery"]["val_tpl"], no_batt) == "None"
    no_power = next(m for m in samples if m["t"] == "status" and "power" not in m)
    assert render(cmps["charging"]["val_tpl"], no_power) == "OFF"
    off = json.loads(json.dumps(st))
    off["bands"]["2.4"]["scan"] = False
    assert render(cmps["alarm_24"]["val_tpl"], off) == "OFF"  # a band that is off never alarms
    assert render(cmps["state_24"]["val_tpl"], off) == "off"
    for name, mask in SCAN_OPTIONS.items():
        st2 = json.loads(json.dumps(st))
        st2["set"]["scan"] = mask
        assert render(cmps["scan"]["val_tpl"], st2) == name


def test_event_payload(samples):
    ev = next(m for m in samples if m["t"] == "event")
    out = discovery.event_payload(ev)
    assert out["event_type"] == "alarm" and out["band"] == "5" and "t" not in out
    assert out["event_type"] in discovery.EVENT_TYPES
