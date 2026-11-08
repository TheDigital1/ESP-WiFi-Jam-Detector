#!/usr/bin/with-contenv bashio
# SPDX-License-Identifier: GPL-3.0-or-later
# Reads the options and the MQTT broker login from the Supervisor, then runs the bridge.

if ! bashio::services.available "mqtt"; then
    bashio::exit.nok "No MQTT broker found. Install and start the Mosquitto broker app (or set up MQTT in Home Assistant) first."
fi

export JD_SERIAL_PORT="$(bashio::config 'device')"
export JD_LOG_LEVEL="$(bashio::config 'log_level')"
export JD_MQTT_HOST="$(bashio::services mqtt 'host')"
export JD_MQTT_PORT="$(bashio::services mqtt 'port')"
export JD_MQTT_USER="$(bashio::services mqtt 'username')"
export JD_MQTT_PASSWORD="$(bashio::services mqtt 'password')"
export JD_MQTT_TLS="$(bashio::services mqtt 'ssl')"

bashio::log.info "Detector on ${JD_SERIAL_PORT}, MQTT broker ${JD_MQTT_HOST}:${JD_MQTT_PORT}"
exec python3 -m jdbridge
