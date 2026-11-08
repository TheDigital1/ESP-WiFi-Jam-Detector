# SPDX-License-Identifier: GPL-3.0-or-later
"""Serial <-> MQTT bridge for one detector.

The main thread owns the serial port (open, read, reconnect); paho's network
thread delivers commands, which are written under a lock. The device is known
once its hello arrives; until then nothing is published for it.
"""

from __future__ import annotations

import json
import logging
import random
import threading
import time
from typing import Callable, Protocol

from . import discovery
from .protocol import PROTOCOL_VERSION, command, parse_line, set_value_from_mqtt

log = logging.getLogger("jdbridge")

RETRY_S = 3.0           # resend the sync / hello command until answered
PING_S = 15.0           # keeps the device streaming (it stops 60 s after the last command)
SILENCE_S = 30.0        # no device message this long: reopen the port
BACKOFF_S = (2, 5, 10, 30)


class SerialLike(Protocol):
    def readline(self) -> bytes: ...
    def write(self, data: bytes) -> int: ...
    def close(self) -> None: ...


class MqttLike(Protocol):
    def publish(self, topic: str, payload: str | bytes | None = None, qos: int = 0, retain: bool = False): ...
    def subscribe(self, topic: str, qos: int = 0): ...
    def unsubscribe(self, topic: str): ...


def open_serial(port: str):
    """Open the detector's port without resetting it (docs/LINK.md): DTR and RTS stay high."""
    import serial

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.dtr = True
    s.rts = True
    s.open()
    return s


class Bridge:
    def __init__(self, mqtt: MqttLike, base_topic: str = "jammer", prefix: str = "homeassistant",
                 clock: Callable[[], float] = time.monotonic):
        self.mqtt = mqtt
        self.base = base_topic
        self.prefix = prefix
        self.clock = clock
        self.serial: SerialLike | None = None
        self.write_lock = threading.Lock()
        self.topics: discovery.Topics | None = None
        self.hello: dict | None = None
        self.params: dict[str, dict] = {}
        self.cmd_id = 0
        self.pending: dict[int, str] = {}
        self.last_msg = 0.0
        self.last_sent = 0.0
        self.sync_id = 0
        self.synced = False
        self.hello_seen = False
        self.warned_version = False

    # ---- MQTT side ----

    @property
    def bridge_availability_topic(self) -> str:
        return f"{self.base}/bridge/availability"

    def on_mqtt_connected(self) -> None:
        """(Re)connected to the broker: announce the bridge and listen."""
        self.mqtt.publish(self.bridge_availability_topic, "online", qos=1, retain=True)
        self.mqtt.subscribe(f"{self.prefix}/status", qos=1)
        if self.topics:
            self.mqtt.subscribe(self.topics.cmd_wildcard, qos=1)
            self._publish_discovery()

    def on_mqtt_message(self, topic: str, payload: bytes) -> None:
        text = payload.decode("utf-8", "replace")
        if topic == f"{self.prefix}/status":
            if text == "online" and self.hello:
                log.info("Home Assistant started: discovery again")
                self._publish_discovery()
            return
        t = self.topics
        if not t or not topic.startswith(t.root + "/cmd/"):
            return
        parts = topic[len(t.root) + 5:].split("/")
        try:
            if parts == ["relearn"]:
                self._send("relearn", "re-learn")
            elif len(parts) == 2 and parts[0] == "set":
                key = parts[1]
                value = set_value_from_mqtt(key, text, self.params)
                self._send("set", f"{key} = {value}", key=key, value=value)
            elif parts == ["get"]:
                self._send("get", "get")
            else:
                log.warning("unknown command topic %s", topic)
        except ValueError as e:
            log.warning("ignored command %s %r: %s", topic, text, e)

    def _send(self, c: str, what: str, key: str | None = None, value=None) -> None:
        self.cmd_id = self.cmd_id % 499999 + 1  # below the sync ids
        self.pending[self.cmd_id] = what
        if len(self.pending) > 50:
            self.pending.pop(next(iter(self.pending)))
        self._write(command(c, self.cmd_id, key, value))

    def _write(self, data: bytes) -> None:
        with self.write_lock:
            if self.serial is None:
                log.warning("device not connected: dropped %s", data.decode().strip())
                return
            try:
                self.serial.write(data)
            except Exception as e:  # the read loop notices and reopens
                log.warning("serial write failed: %s", e)

    def _publish_discovery(self) -> None:
        assert self.topics and self.hello
        payload = discovery.build(self.hello, self.topics)
        self.mqtt.publish(self.topics.discovery, json.dumps(payload, separators=(",", ":")), qos=1, retain=True)
        self.mqtt.publish(self.topics.availability, "online" if self.serial else "offline", qos=1, retain=True)

    # ---- serial side ----

    def attach(self, ser: SerialLike) -> None:
        with self.write_lock:
            self.serial = ser
        self.last_msg = self.clock()
        self.last_sent = 0.0
        # Lines the device wrote while no one read wait in its USB buffer and
        # come first: ignore everything until the answer to this command,
        # whose id the device cannot have seen before.
        self.sync_id = random.randint(500000, 999999)  # device accepts ids up to 1e6
        self.synced = False
        self.hello_seen = False

    def detach(self) -> None:
        with self.write_lock:
            ser, self.serial = self.serial, None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if self.topics:
            self.mqtt.publish(self.topics.availability, "offline", qos=1, retain=True)

    def poll(self) -> bool:
        """Read one line (or time out) and handle it. False when the port should be reopened."""
        assert self.serial is not None
        now = self.clock()
        if not self.synced:
            if now - self.last_sent >= RETRY_S:
                self.last_sent = now
                self._write(command("ping", self.sync_id))
        elif not self.hello_seen:
            if now - self.last_sent >= RETRY_S:
                self.last_sent = now
                self._write(command("hello"))
        elif now - self.last_sent >= PING_S:
            self.last_sent = now
            self._write(command("ping", self.sync_id))
        raw = self.serial.readline()
        if raw:
            msg = parse_line(raw)
            if msg is None:
                text = raw.decode("utf-8", "replace").rstrip()
                if text:
                    log.debug("device log: %s", text)
            else:
                self.last_msg = self.clock()
                self.handle(msg)
        if self.clock() - self.last_msg > SILENCE_S:
            log.warning("no message from the device for %.0f s: reopening the port", SILENCE_S)
            return False
        return True

    def handle(self, msg: dict) -> None:
        if not self.synced:
            if msg.get("t") == "ack" and msg.get("id") == self.sync_id:
                self.synced = True
                self.last_sent = 0.0  # ask for hello now
                log.debug("in sync with the device")
            else:
                log.debug("skipped a buffered %s message", msg.get("t"))
            return
        v = msg.get("v", 0)
        if isinstance(v, int) and v > PROTOCOL_VERSION and not self.warned_version:
            self.warned_version = True
            log.warning("device speaks protocol %s, this bridge %s: update the add-on; known fields still work",
                        v, PROTOCOL_VERSION)
        t = msg["t"]
        if t == "hello":
            self._on_hello(msg)
            return
        if not self.topics or not self.hello_seen:
            return  # nothing is published before we know which device this is
        if t == "status":
            self.mqtt.publish(self.topics.status, json.dumps(msg, separators=(",", ":")), qos=1, retain=True)
        elif t == "event":
            band = discovery.band_slug(str(msg.get("band", "")))
            self.mqtt.publish(self.topics.event(band), json.dumps(discovery.event_payload(msg)), qos=1)
            log.info("%s GHz: %s -> %s (%s)", msg.get("band"), msg.get("from"), msg.get("to"), msg.get("kind"))
        elif t == "ack":
            if msg.get("id") == self.sync_id:
                return  # ping
            what = self.pending.pop(msg.get("id", 0), "command")
            if msg.get("ok"):
                log.info("device: %s done", what)
            else:
                log.warning("device refused %s: %s", what, msg.get("err"))

    def _on_hello(self, msg: dict) -> None:
        dev_id = str(msg.get("id", ""))
        if not dev_id.isalnum():
            log.warning("hello without a usable id: %s", msg)
            return
        self.hello_seen = True
        if self.topics and self.topics.device_id != dev_id:
            log.warning("a different detector is on the port (%s, was %s)", dev_id, self.topics.device_id)
            self.mqtt.publish(self.topics.availability, "offline", qos=1, retain=True)
            self.mqtt.unsubscribe(self.topics.cmd_wildcard)
            self.topics = None
        first = self.topics is None or self.hello != msg
        if self.topics is None:
            self.topics = discovery.Topics(self.base, dev_id, self.prefix)
            self.mqtt.subscribe(self.topics.cmd_wildcard, qos=1)
        self.hello = msg
        self.params = {p["k"]: p for p in msg.get("params", [])}
        if first:
            log.info("detector %s, firmware %s, protocol %s", dev_id, msg.get("fw"), msg.get("v"))
        self._publish_discovery()

    def run_serial(self, port: str, stop: threading.Event, opener=open_serial) -> None:
        """Keep the port open and serviced until stop is set."""
        attempt = 0
        while not stop.is_set():
            try:
                ser = opener(port)
            except Exception as e:
                delay = BACKOFF_S[min(attempt, len(BACKOFF_S) - 1)]
                log.warning("cannot open %s (%s): retry in %d s", port, e, delay)
                attempt += 1
                stop.wait(delay)
                continue
            log.info("opened %s", port)
            self.attach(ser)
            try:
                while not stop.is_set() and self.poll():
                    if self.hello_seen:
                        attempt = 0
            except Exception as e:
                log.warning("serial error on %s: %s", port, e)
            self.detach()
            if not stop.is_set():
                delay = BACKOFF_S[min(attempt, len(BACKOFF_S) - 1)]
                attempt += 1
                stop.wait(delay)
