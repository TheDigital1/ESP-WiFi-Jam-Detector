# SPDX-License-Identifier: GPL-3.0-or-later
"""python -m jdbridge --port /dev/serial/by-id/... --mqtt-host core-mosquitto ..."""

from __future__ import annotations

import argparse
import logging
import os
import signal
import ssl
import threading

import paho.mqtt.client as paho

from . import __version__
from .bridge import Bridge

log = logging.getLogger("jdbridge")


def parse_args(argv=None) -> argparse.Namespace:
    env = os.environ.get
    p = argparse.ArgumentParser(prog="jdbridge", description="esp-wifi-jammer-detector USB to MQTT bridge")
    p.add_argument("--port", default=env("JD_SERIAL_PORT"), help="serial port of the detector")
    p.add_argument("--mqtt-host", default=env("JD_MQTT_HOST", "localhost"))
    p.add_argument("--mqtt-port", type=int, default=int(env("JD_MQTT_PORT", "1883")))
    p.add_argument("--mqtt-user", default=env("JD_MQTT_USER") or None)
    p.add_argument("--mqtt-password", default=env("JD_MQTT_PASSWORD") or None)
    p.add_argument("--mqtt-tls", action="store_true", default=env("JD_MQTT_TLS", "false").lower() == "true")
    p.add_argument("--base-topic", default=env("JD_BASE_TOPIC", "jammer"))
    p.add_argument("--discovery-prefix", default=env("JD_DISCOVERY_PREFIX", "homeassistant"))
    p.add_argument("--log-level", default=env("JD_LOG_LEVEL", "info"),
                   choices=["debug", "info", "warning", "error"])
    a = p.parse_args(argv)
    if not a.port:
        p.error("--port (or JD_SERIAL_PORT) is required")
    return a


def main(argv=None) -> None:
    a = parse_args(argv)
    logging.basicConfig(level=a.log_level.upper(), format="%(asctime)s %(levelname)s %(message)s")
    log.info("jammer-detector-bridge %s: %s -> mqtt://%s:%d", __version__, a.port, a.mqtt_host, a.mqtt_port)

    client = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"jammer-detector-bridge-{os.getpid()}")
    bridge = Bridge(client, base_topic=a.base_topic, prefix=a.discovery_prefix)
    client.will_set(bridge.bridge_availability_topic, "offline", qos=1, retain=True)
    if a.mqtt_user:
        client.username_pw_set(a.mqtt_user, a.mqtt_password)
    if a.mqtt_tls:
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    def on_connect(c, userdata, flags, reason, props):
        if reason.is_failure:
            log.error("MQTT connect failed: %s", reason)
            return
        log.info("connected to MQTT broker")
        bridge.on_mqtt_connected()

    def on_disconnect(c, userdata, flags, reason, props):
        if reason.is_failure:
            log.warning("MQTT connection lost (%s): reconnecting", reason)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = lambda c, u, m: bridge.on_mqtt_message(m.topic, m.payload)
    client.connect_async(a.mqtt_host, a.mqtt_port, keepalive=30)
    client.loop_start()

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    try:
        bridge.run_serial(a.port, stop)
    finally:
        bridge.detach()
        try:
            client.publish(bridge.bridge_availability_topic, "offline", qos=1, retain=True).wait_for_publish(2)
        except (RuntimeError, ValueError):
            pass  # not connected: the broker sends the will instead
        client.loop_stop()
        client.disconnect()
        log.info("stopped")


if __name__ == "__main__":
    main()
