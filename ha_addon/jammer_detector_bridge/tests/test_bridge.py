# SPDX-License-Identifier: GPL-3.0-or-later
"""The bridge against a simulated device and broker."""
import json
import threading

from jdbridge import bridge as bridge_mod
from jdbridge.bridge import Bridge


class FakeMqtt:
    def __init__(self):
        self.pub: list[tuple[str, str, bool]] = []
        self.subs: set[str] = set()

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.pub.append((topic, payload, retain))

    def subscribe(self, topic, qos=0):
        self.subs.add(topic)

    def unsubscribe(self, topic):
        self.subs.discard(topic)

    def last(self, topic):
        return next((p for t, p, _ in reversed(self.pub) if t == topic), None)

    def topics(self):
        return [t for t, _, _ in self.pub]


class FakeDevice:
    """Answers like the firmware: ack to ping/get/set, hello to hello. Starts with stale lines buffered."""

    def __init__(self, hello: dict, stale: list[bytes] = ()):
        self.hello = hello
        self.out: list[bytes] = list(stale)
        self.got: list[dict] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        for line in data.decode().splitlines():
            cmd = json.loads(line)
            self.got.append(cmd)
            if cmd["c"] == "hello":
                self.out.append(json.dumps(self.hello).encode() + b"\r\n")
            else:
                ok = not (cmd["c"] == "set" and cmd.get("k") == "nope")
                ack = {"t": "ack", "v": 1, "id": cmd.get("id", 0), "ok": ok}
                if not ok:
                    ack["err"] = "bad key or value"
                self.out.append(json.dumps(ack).encode() + b"\r\n")
        return len(data)

    def readline(self) -> bytes:
        return self.out.pop(0) if self.out else b""

    def close(self):
        self.closed = True

    def send(self, msg: dict):
        self.out.append(b"I (1) log line first\r\n" + json.dumps(msg).encode() + b"\r\n")


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def make(hello, stale=()):
    mqtt, clock = FakeMqtt(), Clock()
    b = Bridge(mqtt, clock=clock)
    dev = FakeDevice(hello, stale)
    b.attach(dev)
    return b, mqtt, dev, clock


def run(b, n=10):
    for _ in range(n):
        assert b.poll()


def status(samples):
    return next(m for m in samples if m["t"] == "status")


def test_stale_lines_ignored_until_sync(hello, samples):
    stale_alarm = {"t": "event", "v": 1, "band": "2.4", "from": "suspect", "to": "alarm", "kind": "wideband"}
    old = dict(hello, id="aaaaaaaaaaaa")
    stale = [json.dumps(old).encode(), json.dumps(stale_alarm).encode(), json.dumps(status(samples)).encode()]
    b, mqtt, dev, _ = make(hello, stale)
    run(b)
    assert dev.got[0]["c"] == "ping" and dev.got[0]["id"] == b.sync_id
    assert dev.got[1]["c"] == "hello"
    assert b.topics.device_id == hello["id"]
    assert not any("/event/" in t for t in mqtt.topics()), "a buffered event reached MQTT"
    assert not any("aaaaaaaaaaaa" in t for t in mqtt.topics())


def test_discovery_and_state(hello, samples):
    b, mqtt, dev, _ = make(hello)
    run(b)
    root = f"jammer/{hello['id']}"
    disc = json.loads(mqtt.last(f"homeassistant/device/jammer_detector_{hello['id']}/config"))
    assert "alarm_24" in disc["cmps"]
    assert mqtt.last(f"{root}/availability") == "online"
    assert f"{root}/cmd/#" in mqtt.subs

    dev.send(status(samples))
    ev = next(m for m in samples if m["t"] == "event")
    dev.send(ev)
    run(b)
    assert json.loads(mqtt.last(f"{root}/status"))["bands"]["2.4"]["state"] == "alarm"
    assert all(r for t, _, r in mqtt.pub if t == f"{root}/status")  # retained
    e = json.loads(mqtt.last(f"{root}/event/5"))
    assert e["event_type"] == "alarm" and e["kind"] == "wideband"
    assert not any(r for t, _, r in mqtt.pub if "/event/" in t)  # events are not retained


def test_commands(hello):
    b, mqtt, dev, _ = make(hello)
    run(b)
    root = f"jammer/{hello['id']}"
    n = len(dev.got)
    b.on_mqtt_message(f"{root}/cmd/set/beep", b"ON")
    b.on_mqtt_message(f"{root}/cmd/set/scan", b"5 GHz")
    b.on_mqtt_message(f"{root}/cmd/set/wide_db", b"8.0")
    b.on_mqtt_message(f"{root}/cmd/relearn", b"PRESS")
    b.on_mqtt_message(f"{root}/cmd/set/wide_db", b"999")  # out of range: not sent
    b.on_mqtt_message("jammer/otherdevice/cmd/relearn", b"PRESS")  # not ours
    sent = dev.got[n:]
    assert [(c["c"], c.get("k"), c.get("v")) for c in sent] == [
        ("set", "beep", True), ("set", "scan", 2), ("set", "wide_db", 8), ("relearn", None, None)]
    run(b)
    assert not b.pending  # every command acknowledged


def test_ha_restart_republishes(hello):
    b, mqtt, dev, _ = make(hello)
    run(b)
    topic = f"homeassistant/device/jammer_detector_{hello['id']}/config"
    count = mqtt.topics().count(topic)
    b.on_mqtt_message("homeassistant/status", b"online")
    assert mqtt.topics().count(topic) == count + 1


def test_ping_and_silence(hello, samples):
    b, mqtt, dev, clock = make(hello)
    run(b)
    pings = sum(1 for c in dev.got if c["c"] == "ping")
    clock.t += bridge_mod.PING_S + 1
    dev.send(status(samples))
    run(b, 3)
    assert sum(1 for c in dev.got if c["c"] == "ping") == pings + 1
    dev.write = lambda data: len(data)  # device stops answering (hung or wrong port)
    clock.t += bridge_mod.SILENCE_S + 1
    assert b.poll() is False  # nothing for too long: reopen


def test_unplug_marks_offline(hello):
    b, mqtt, dev, _ = make(hello)
    run(b)
    b.detach()
    assert dev.closed
    assert mqtt.last(f"jammer/{hello['id']}/availability") == "offline"


def test_run_serial_reconnects(hello, monkeypatch):
    monkeypatch.setattr(bridge_mod, "BACKOFF_S", (0,))
    mqtt = FakeMqtt()
    b = Bridge(mqtt)
    stop = threading.Event()
    opened = []

    def opener(port):
        if len(opened) == 0:
            opened.append("fail")
            raise OSError("no such device")
        dev = FakeDevice(hello)
        opened.append(dev)
        if len(opened) == 3:
            stop.set()

        class Broken(FakeDevice):
            def readline(self_inner):
                line = FakeDevice.readline(self_inner)
                if not line and b.hello_seen:
                    raise OSError("device unplugged")
                return line

        broken = Broken(hello)
        opened[-1] = broken
        return broken

    b.run_serial("/dev/ttyACM9", stop, opener=opener)
    assert len(opened) == 3 and opened[0] == "fail"
    assert mqtt.last(f"jammer/{hello['id']}/availability") == "offline"
