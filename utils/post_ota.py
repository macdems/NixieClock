#!/usr/bin/env python3
"""
Announce new firmware via MQTT and serve it over HTTP until the ESP downloads it.

Requires: pip install paho-mqtt
"""
import argparse
import hashlib
import logging
import os
import re
import socket
import ssl
import sys
import threading
import time
from importlib.metadata import version as package_version
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from colorlog import LevelColorFormatter

# Module-level loggers for readability and consistency
LOG = logging.getLogger("post_ota")
HTTP_LOG = logging.getLogger("post_ota.http")

try:
    import paho.mqtt.client as mqtt
except Exception:
    LOG.error("paho-mqtt not installed. Run: python -m pip install paho-mqtt")
    raise

try:
    from tqdm import trange
except ImportError:
    trange = None  # type: ignore

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


def detect_primary_ip():
    ip = "127.0.0.1"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
    except Exception:
        pass
    return ip


class OneFileHandler(BaseHTTPRequestHandler):
    allowed_path = ""
    file_path: Path = None  # type: ignore
    download_started_event: threading.Event = None  # type: ignore
    download_finished_event: threading.Event = None  # type: ignore
    server_version = "ESP-OTA-Server/1.0"
    sys_version = ""

    def log_message(self, format, *args):
        HTTP_LOG.debug(format % args)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path != self.allowed_path:
            self.send_error(404, "Not Found")
            return
        try:
            if self.download_started_event:
                self.download_started_event.set()
            fs = self.file_path.stat()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(fs.st_size))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            sent = 0
            if trange:
                progress = trange(fs.st_size, unit="B", unit_scale=True, desc="Sending", leave=False)
            else:
                progress = None
            with self.file_path.open("rb") as f:
                while True:
                    chunk = f.read(16 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    if progress:
                        progress.update(len(chunk))
            if progress:
                progress.close()
            self.log_message("Served %s (%d bytes) to %s", self.allowed_path, fs.st_size, self.client_address[0])
            if self.download_finished_event and sent == fs.st_size:
                self.download_finished_event.set()
        except BrokenPipeError:
            self.log_message("Client closed early: %s", self.client_address[0])
        except Exception as e:
            self.send_error(500, f"Server error: {e}")


def serve_until_download(bind_host, port, url_path, file_path, timeout_sec, mqtt_client=None, *, certfile: str, keyfile: str):
    download_started = threading.Event()
    download_finished = threading.Event()
    OneFileHandler.allowed_path = url_path
    OneFileHandler.file_path = Path(file_path)
    OneFileHandler.download_started_event = download_started
    OneFileHandler.download_finished_event = download_finished

    httpd = ThreadingHTTPServer((bind_host, port), OneFileHandler)
    httpd.timeout = 1.0

    # Wrap server socket with TLS
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=certfile, keyfile=keyfile)
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    except FileNotFoundError as e:
        LOG.error("TLS certificate or key not found: %s", e)
        raise
    except ssl.SSLError as e:
        LOG.error("Failed to initialize TLS: %s", e)
        raise

    LOG.info("HTTPS on https://%s:%d%s", bind_host, port, url_path)
    LOG.info("Waiting up to %ss for ESP to download…", timeout_sec)

    start = time.time()
    last_publish = start
    success = False
    try:
        while True:
            httpd.handle_request()
            if download_finished.is_set():
                success = True
                break
            now = time.time()
            if (now - start) > timeout_sec: break
            if mqtt_client is not None and not download_started.is_set():
                # Publish install command again every 10 seconds
                if now - last_publish >= 10:
                    FW_INSTALL_TOPIC = f"{NODE_NAMESPACE}/firmware/set"
                    fw_url = f"https://{detect_primary_ip()}:{port}{url_path}"
                    mqtt_client.publish(FW_INSTALL_TOPIC, fw_url, qos=2, retain=False).wait_for_publish()
                    LOG.info("Re-published install command '%s'.", fw_url)
                    last_publish = now

    finally:
        # if success and keepalive_sec > 0:
        #     print(f"[post_ota] Waiting for {keepalive_sec}s before finish…")
        #     end_keep = time.time() + keepalive_sec
        #     while time.time() < end_keep:
        #         httpd.handle_request()
        LOG.info("Stopping HTTPS server.")
        httpd.server_close()
    return success


def on_message(client, userdata, msg):
    print(msg.payload.decode())


def main():
    # TLS cert/key
    # Defaults to repo ssl/ directory next to this script's parent
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    default_cert = str((repo_root / "ssl" / "server_cert.pem").resolve())
    default_key = str((repo_root / "ssl" / "server_key.pem").resolve())

    p = argparse.ArgumentParser(description="Announce ESP OTA and serve firmware until downloaded.")
    p.add_argument("-d", "--build-dir", default="build", help="IDF build directory")
    p.add_argument("-b", "--bin", default=None, help="Explicit path to firmware .bin (optional)")
    p.add_argument("-n", "--node-prefix", default=NODE_NAMESPACE, help="MQTT node prefix")
    p.add_argument("-H", "--mqtt-host", default=MQTT_HOST, help="MQTT broker host")
    p.add_argument("-P", "--mqtt-port", type=int, default=MQTT_PORT, help="MQTT broker port")
    p.add_argument("-u", "--mqtt-user", default=MQTT_USERNAME, help="MQTT username")
    p.add_argument("-p", "--mqtt-pass", default=MQTT_PASSWORD, help="MQTT password")
    p.add_argument("-s", "--mqtt-tls", action="store_true", help="Use MQTT over TLS")
    p.add_argument("-c", "--mqtt-ca", default=None, help="Path to MQTT CA certificate file")
    p.add_argument("-U", "--fw-base-url", default="", help="Base URL for firmware (if not serving locally)")
    p.add_argument("--bind-host", default="0.0.0.0", help="HTTPS server bind host")
    p.add_argument("--bind-port", type=int, default=8443, help="HTTPS server bind port")
    p.add_argument("--cert", default=default_cert, help="Path to TLS certificate (PEM), default: ssl/server_cert.pem")
    p.add_argument("--key", default=default_key, help="Path to TLS private key (PEM), default: ssl/server_key.pem")
    p.add_argument("-i", "--publish-install", action="store_true", help="Publish install command after announcing new firmware")
    p.add_argument("-t", "--serve-timeout", type=int, default=300, help="Seconds to serve HTTP until download (0 to skip serving)")
    # p.add_argument("-a", "--serve-keepalive", type=int, default=10, help="Seconds to keep HTTP server alive after download")
    p.add_argument("-v", "--project-ver", default=None, help="Firmware version tag (default: timestamp)")
    p.add_argument("-w", "--watch-logs", action="store_true", help="Watch MQTT debug logs after OTA")
    p.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="Logging level (default: INFO)"
    )
    p.add_argument("--no-color", action="store_true", help="Disable ANSI color in logs")
    args = p.parse_args()

    # Configure logging with optional ANSI colors
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

    build_dir = Path(args.build_dir)
    if args.bin:
        firmware_bin = Path(args.bin)
    else:
        # Try to find <project>.bin in build dir
        bins = sorted(build_dir.glob("*.bin"))
        if not bins:
            LOG.error("No .bin found in %s. Pass --bin explicitly.", build_dir)
            sys.exit(2)
        # Prefer the app image (exclude bootloader & partition-table if present)
        app_bins = [b for b in bins if b.name not in ("bootloader.bin", "partition-table.bin")]
        firmware_bin = app_bins[0] if app_bins else bins[0]
    out_dir = Path("out") / "firmware"
    out_dir.mkdir(parents=True, exist_ok=True)

    # Determine version tag
    ver = args.project_ver or ""
    fw_name = f"{NODE_NAMESPACE}-{ver}.bin" if ver else f"{NODE_NAMESPACE}.bin"
    data = firmware_bin.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    size = firmware_bin.stat().st_size
    LOG.info("Firmware: %s (%d bytes, sha256 %s…)", firmware_bin, size, sha[:8])

    # URL build
    if args.fw_base_url:
        base = args.fw_base_url.rstrip('/')
        fw_url = f"{base}/{fw_name}"
        url_path = "/" + "/".join(urlparse(fw_url).path.split("/")[1:])
    else:
        ip = detect_primary_ip()
        fw_url = f"https://{ip}:{args.bind_port}/firmware/{fw_name}"
        url_path = f"/firmware/{fw_name}"

    # FW_LATEST_VER_TOPIC = f"{args.node_prefix}/firmware/latest_version"
    # FW_LATEST_URL_TOPIC = f"{args.node_prefix}/firmware/latest_url"
    FW_INSTALL_TOPIC = f"{args.node_prefix}/firmware/set"

    mqtt_version = tuple(int(x) for x in package_version("paho-mqtt").split(".")[:2])
    if mqtt_version >= (2, 0):
        mqtt_client = mqtt.Client(client_id=f"idf-ota-{NODE_NAMESPACE or int(time.time())}", callback_api_version=2)
    else:
        mqtt_client = mqtt.Client(client_id=f"idf-ota-{NODE_NAMESPACE or int(time.time())}")

    if args.mqtt_user:
        mqtt_client.username_pw_set(args.mqtt_user, args.mqtt_pass or None)
    if args.mqtt_tls:
        if args.mqtt_ca and os.path.isfile(args.mqtt_ca): mqtt_client.tls_set(ca_certs=args.mqtt_ca)
        else: mqtt_client.tls_set()
    if args.watch_logs:
        mqtt_client.on_message = on_message
    mqtt_client.connect(args.mqtt_host, args.mqtt_port, keepalive=30)
    mqtt_client.loop_start()

    # client.publish(FW_LATEST_VER_TOPIC, ver, qos=1, retain=False).wait_for_publish()
    # client.publish(FW_LATEST_URL_TOPIC, fw_url, qos=1, retain=False).wait_for_publish()
    # LOG.info("Published latest_version='%s', latest_url=%s", ver, fw_url)
    if args.publish_install:
        mqtt_client.publish(FW_INSTALL_TOPIC, fw_url, qos=2, retain=False).wait_for_publish()
        LOG.info("Published install command '%s'.", fw_url)

    if args.serve_timeout > 0:
        try:
            ok = serve_until_download(
                args.bind_host,
                args.bind_port,
                url_path,
                firmware_bin,
                args.serve_timeout,
                mqtt_client if args.publish_install else None,
                certfile=args.cert,
                keyfile=args.key,
            )
            if ok:
                LOG.info("ESP download finished.")
            else:
                LOG.warning("Timeout waiting for download.")
        except OSError as e:
            LOG.error("HTTP server error on %s:%d: %s", args.bind_host, args.bind_port, e)
    else:
        LOG.info("Skipping local HTTP serving (serve-timeout=0).")

    if args.watch_logs:
        LOG.info("Starting to watch MQTT debug logs…")
        topic = f"{args.node_prefix}/debug"
        LOG.info("Subscribing to MQTT topic: %s", topic)
        mqtt_client.subscribe(topic, 1)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            LOG.info("Stopping MQTT log watch.")

    mqtt_client.loop_stop()
    mqtt_client.disconnect()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        LOG.info("Interrupted by user.")
        sys.exit(0)
