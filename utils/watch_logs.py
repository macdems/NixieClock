#!/usr/bin/env python3
"""
Watch MQTT debug logs from the device.

Requires: pip install paho-mqtt
"""
import argparse
import logging
import os
import re
import sys
import time
from importlib.metadata import version as package_version

from colorlog import LevelColorFormatter

# Module-level logger
LOG = logging.getLogger("watch_logs")

try:
    import paho.mqtt.client as mqtt
except Exception:
    LOG.error("paho-mqtt not installed. Run: python -m pip install paho-mqtt")
    raise

try:
    import kconfiglib
    kconf = kconfiglib.Kconfig("main/Kconfig.projbuild")
    kconf.load_config("sdkconfig")
except Exception:
    BROKER_URI = MQTT_USERNAME = MQTT_PASSWORD = ""
    NODE_NAMESPACE = "esp32"
else:
    BROKER_URI = kconf.syms["BROKER_URI"].str_value
    MQTT_USERNAME = kconf.syms["MQTT_USERNAME"].str_value
    MQTT_PASSWORD = kconf.syms["MQTT_PASSWORD"].str_value
    NODE_NAMESPACE = kconf.syms["NODE_NAMESPACE"].str_value

MQTT_HOST, MQTT_PORT = "127.0.0.1", 1883
if BROKER_URI:
    # Parse BROKER_URI inline (e.g., mqtt://user:pass@host:port or tcp://host:port)
    m = re.match(r'^(?:\w+://)?(?:[^@]+@)?([^:/]+)(?::(\d+))?', BROKER_URI)
    if m:
        MQTT_HOST = m.group(1)
        MQTT_PORT = int(m.group(2)) if m.group(2) else 1883


def on_message(client, userdata, msg):
    print(msg.payload.decode())


def main():
    p = argparse.ArgumentParser(description="Announce ESP OTA and serve firmware until downloaded.")
    p.add_argument("-n", "--node-prefix", default=NODE_NAMESPACE, help="MQTT node prefix")
    p.add_argument("-H", "--mqtt-host", default=MQTT_HOST, help="MQTT broker host")
    p.add_argument("-P", "--mqtt-port", type=int, default=MQTT_PORT, help="MQTT broker port")
    p.add_argument("-u", "--mqtt-user", default=MQTT_USERNAME, help="MQTT username")
    p.add_argument("-p", "--mqtt-pass", default=MQTT_PASSWORD, help="MQTT password")
    p.add_argument("-s", "--mqtt-tls", action="store_true", help="Use MQTT over TLS")
    p.add_argument("-c", "--mqtt-ca", default=None, help="Path to MQTT CA certificate file")
    p.add_argument("-l", "--log-level", default="INFO", choices=["DEBUG","INFO","WARNING","ERROR","CRITICAL"], help="Logging level")
    p.add_argument("--no-color", action="store_true", help="Disable ANSI color in logs")
    args = p.parse_args()

    # Logging with optional color using shared formatter
    level = getattr(logging, args.log_level.upper(), logging.INFO)
    logging.basicConfig(level=level)
    fmt = "[%(name)s] %(levelname)s: %(message)s"
    handler = logging.StreamHandler()
    handler.setLevel(level)
    handler.setFormatter(LevelColorFormatter(fmt, use_color=not args.no_color))
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    root.addHandler(handler)

    mqtt_version = tuple(int(x) for x in package_version("paho-mqtt").split(".")[:2])
    if mqtt_version >= (2, 0):
        mqtt_client = mqtt.Client(client_id=f"idf-logs-{NODE_NAMESPACE or int(time.time())}", callback_api_version=2)
    else:
        mqtt_client = mqtt.Client(client_id=f"idf-logs-{NODE_NAMESPACE or int(time.time())}")

    if args.mqtt_user:
        mqtt_client.username_pw_set(args.mqtt_user, args.mqtt_pass or None)
    if args.mqtt_tls:
        if args.mqtt_ca and os.path.isfile(args.mqtt_ca): mqtt_client.tls_set(ca_certs=args.mqtt_ca)
        else: mqtt_client.tls_set()

    topic = f"{args.node_prefix}/debug"
    mqtt_client.on_message = on_message
    mqtt_client.connect(args.mqtt_host, args.mqtt_port, keepalive=60)
    LOG.info("Subscribing to MQTT topic: %s", topic)
    mqtt_client.subscribe(topic, 2)
    mqtt_client.loop_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
