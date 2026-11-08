# SPDX-License-Identifier: GPL-3.0-or-later
import json
import pathlib
import sys

import pytest

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE.parent))

from jdbridge.protocol import parse_line  # noqa: E402


@pytest.fixture
def samples() -> list[dict]:
    """One of each message, as the firmware formats them (test_host/test_link.c writes these)."""
    return [json.loads(l) for l in (HERE / "fixtures" / "link_samples.jsonl").read_text().splitlines()]


@pytest.fixture
def capture() -> bytes:
    """25 s from the real device: console log and messages mixed."""
    return (HERE / "fixtures" / "serial_capture.txt").read_bytes()


@pytest.fixture
def hello(capture) -> dict:
    return next(m for m in map(parse_line, capture.splitlines()) if m and m["t"] == "hello")
