# SPDX-License-Identifier: GPL-3.0-or-later
import json

import pytest

from jdbridge.protocol import command, parse_line, set_value_from_mqtt


def test_capture_messages_and_logs(capture):
    msgs = [m for m in map(parse_line, capture.splitlines()) if m]
    kinds = {m["t"] for m in msgs}
    assert {"hello", "status", "ack"} <= kinds
    assert all(m["v"] == 1 for m in msgs)
    logs = [l for l in capture.splitlines() if l.strip() and parse_line(l) is None]
    assert any(b"sweep:" in l for l in logs)  # console lines are skipped, not misread


def test_marker_after_cut_log_line():
    line = b'I (3187) lvgl: LVGL 9.5.0 up, 480x{"t":"ack","v":1,"id":3,"ok":true}\r\n'
    assert parse_line(line) == {"t": "ack", "v": 1, "id": 3, "ok": True}


@pytest.mark.parametrize("line", [
    b"", b"plain log", b'{"t":"status","v":1,"bands":{"2.4"', b'{"t":5}', b'{"c":"hello"}', b"\xff\xfe{\"t\":",
])
def test_rejects(line):
    assert parse_line(line) is None


def test_command_format():
    assert command("hello") == b'{"c":"hello"}\n'
    assert json.loads(command("set", 7, "beep", True)) == {"c": "set", "k": "beep", "v": True, "id": 7}


def test_set_values(hello):
    params = {p["k"]: p for p in hello["params"]}
    assert set_value_from_mqtt("beep", "ON", params) is True
    assert set_value_from_mqtt("beep", "OFF", params) is False
    assert set_value_from_mqtt("scan", "Both", params) == 3
    assert set_value_from_mqtt("scan", "5 GHz", params) == 2
    assert set_value_from_mqtt("wide_db", "8.0", params) == 8
    assert set_value_from_mqtt("learn_s", "64", params) == 60  # nearest step
    with pytest.raises(ValueError):
        set_value_from_mqtt("wide_db", "99", params)
    with pytest.raises(ValueError):
        set_value_from_mqtt("nope", "1", params)
    with pytest.raises(ValueError):
        set_value_from_mqtt("scan", "both", params)
    with pytest.raises(ValueError):
        set_value_from_mqtt("wide_db", "abc", params)
