# SPDX-License-Identifier: GPL-3.0-or-later
"""The detector's USB link: JSON lines mixed with console log (docs/LINK.md)."""

from __future__ import annotations

import json

PROTOCOL_VERSION = 1
MARKER = '{"t":'

# "scan" setting: bit 0 = 2.4 GHz, bit 1 = 5 GHz.
SCAN_OPTIONS = {"2.4 GHz": 1, "5 GHz": 2, "Both": 3}


def parse_line(raw: bytes | str) -> dict | None:
    """A device message, or None for log lines and anything that does not parse.

    The message may follow the start of a log line cut short by a reset, so the
    marker is looked for anywhere in the line.
    """
    line = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    i = line.find(MARKER)
    if i < 0:
        return None
    try:
        msg = json.loads(line[i:].strip())
    except ValueError:
        return None
    if not isinstance(msg, dict) or not isinstance(msg.get("t"), str):
        return None
    return msg


def command(c: str, cmd_id: int = 0, key: str | None = None, value: int | bool | None = None) -> bytes:
    """One command line for the device."""
    obj: dict = {"c": c}
    if key is not None:
        obj["k"] = key
    if value is not None:
        obj["v"] = value
    if cmd_id:
        obj["id"] = cmd_id
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode()


def set_value_from_mqtt(key: str, payload: str, params: dict[str, dict]) -> int | bool:
    """Convert an MQTT command payload for setting `key` to the device's value.

    Raises ValueError if the payload does not fit the setting.
    """
    payload = payload.strip()
    if key == "beep":
        if payload.upper() in ("ON", "TRUE", "1"):
            return True
        if payload.upper() in ("OFF", "FALSE", "0"):
            return False
        raise ValueError(f"beep: {payload!r}")
    if key == "scan":
        if payload in SCAN_OPTIONS:
            return SCAN_OPTIONS[payload]
        raise ValueError(f"scan: {payload!r}")
    p = params.get(key)
    if p is None:
        raise ValueError(f"unknown setting {key!r}")
    v = float(payload)
    step = p["step"]
    n = round((v - p["min"]) / step)
    iv = int(p["min"] + n * step)
    if not p["min"] <= iv <= p["max"]:
        raise ValueError(f"{key}: {payload} outside {p['min']}..{p['max']}")
    return iv
