#!/usr/bin/env python3
"""Transfer ESP-IDF firmware using the existing pull or authenticated push OTA.

Pull mode delegates to the repository's existing utils/post_ota.py.
Push mode uses the NIXO v1 AES-256-GCM TCP protocol implemented in main/ota.c.

For push mode, install the Python crypto dependency with:
    python -m pip install cryptography
"""

import argparse
import hashlib
import logging
import os
import re
import socket
from urllib.parse import urlparse
from pathlib import Path

try:
    from tqdm import trange
except ImportError:
    trange = None  # type: ignore

LOG = logging.getLogger("ota")
CHUNK_SIZE = 16 * 1024
MAX_IMAGE_SIZE = 2 * 1024 * 1024
HEADER_SIZE = 21
TAG_SIZE = 16


def read_config():
    """Return OTA_PORT, OTA_KEY and MQTT settings from sdkconfig."""
    repo_root = Path(__file__).resolve().parents[1]
    port = 3232
    key = ""
    mqtt_uri = "mqtt://127.0.0.1:1883"
    mqtt_user = mqtt_password = ""
    node_namespace = "esp32"
    sdkconfig = repo_root / "sdkconfig"

    try:
        import kconfiglib

        kconf = kconfiglib.Kconfig(str(repo_root / "main" / "Kconfig.projbuild"))
        if sdkconfig.exists():
            kconf.load_config(str(sdkconfig))
        if "OTA_PORT" in kconf.syms:
            port = int(kconf.syms["OTA_PORT"].str_value or port)
        if "OTA_KEY" in kconf.syms:
            key = kconf.syms["OTA_KEY"].str_value or ""
        mqtt_uri = kconf.syms["BROKER_URI"].str_value or mqtt_uri
        mqtt_user = kconf.syms["MQTT_USERNAME"].str_value or ""
        mqtt_password = kconf.syms["MQTT_PASSWORD"].str_value or ""
        node_namespace = kconf.syms["NODE_NAMESPACE"].str_value or node_namespace
        return port, key, mqtt_uri, mqtt_user, mqtt_password, node_namespace
    except Exception as exc:
        LOG.debug("Cannot load Kconfig values (%s); trying sdkconfig directly", exc)

    if sdkconfig.exists():
        try:
            content = sdkconfig.read_text(encoding="utf-8")
            match = re.search(r"^CONFIG_OTA_PORT=(\d+)\s*$", content, re.MULTILINE)
            if match:
                port = int(match.group(1))
            match = re.search(r'^CONFIG_OTA_KEY="([^"\n]*)"\s*$', content, re.MULTILINE)
            if match:
                key = match.group(1)
            for config_name, variable in (("BROKER_URI", "mqtt_uri"), ("MQTT_USERNAME", "mqtt_user"), ("MQTT_PASSWORD", "mqtt_password"), ("NODE_NAMESPACE", "node_namespace")):
                match = re.search(r'^CONFIG_' + config_name + r'="([^"\n]*)"\s*$', content, re.MULTILINE)
                if match:
                    if variable == "mqtt_uri": mqtt_uri = match.group(1)
                    elif variable == "mqtt_user": mqtt_user = match.group(1)
                    elif variable == "mqtt_password": mqtt_password = match.group(1)
                    else: node_namespace = match.group(1)
        except OSError as exc:
            LOG.debug("Cannot read %s: %s", sdkconfig, exc)
    return port, key, mqtt_uri, mqtt_user, mqtt_password, node_namespace


def mqtt_connection_settings(uri: str):
    """Parse the broker URI formats used by the ESP configuration."""
    if "://" not in uri:
        uri = "mqtt://" + uri
    parsed = urlparse(uri)
    scheme = parsed.scheme.lower()
    tls = scheme in ("mqtts", "mqtt+tls", "ssl", "tls")
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or (8883 if tls else 1883)
    return host, port, tls


def wait_for_device_ip(mqtt_host: str, mqtt_port: int, mqtt_tls: bool, mqtt_user: str, mqtt_password: str,
                       node_prefix: str, timeout: float) -> tuple[str, str]:
    """Wait for the retained IP published by the ESP under <node-id>/ip/state."""
    try:
        import paho.mqtt.client as mqtt
    except ImportError as exc:
        raise RuntimeError("Finding the device IP via MQTT requires paho-mqtt. Install it with: python -m pip install paho-mqtt") from exc

    import threading
    found = threading.Event()
    result = {"ip": "", "topic": ""}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        # Support both the Paho MQTT 2.x and older callback signatures.
        if hasattr(reason_code, "value") and reason_code.value != 0:
            return
        if isinstance(reason_code, int) and reason_code != 0:
            return
        client.subscribe([(f"{node_prefix}/ip/state", 1), (f"{node_prefix}/+/ip/state", 1)])

    def on_message(client, userdata, msg):
        ip = msg.payload.decode("utf-8", errors="replace").strip()
        # Only accept an IPv4 literal; never treat arbitrary MQTT payload as a hostname.
        try:
            import ipaddress
            parsed_ip = ipaddress.ip_address(ip)
            if parsed_ip.version != 4 or parsed_ip.is_unspecified or parsed_ip.is_multicast:
                return
        except ValueError:
            return
        result["ip"] = str(parsed_ip)
        result["topic"] = msg.topic
        found.set()

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except (AttributeError, TypeError):
        client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    if mqtt_user:
        client.username_pw_set(mqtt_user, mqtt_password or None)
    if mqtt_tls:
        client.tls_set()

    LOG.info("Waiting for device IP on MQTT topic %s/ip/state (timeout %ss)…", node_prefix, int(timeout))
    try:
        client.connect(mqtt_host, mqtt_port, keepalive=30)
        client.loop_start()
        if not found.wait(timeout):
            raise TimeoutError(
                f"No device IP received from MQTT in {timeout:g}s. Check the broker, --node-prefix, and that the device publishes <node-id>/ip/state."
            )
        LOG.info("Device IP received from %s: %s", result["topic"], result["ip"])
        return result["ip"], result["topic"]
    finally:
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass


def select_firmware(build_dir: str, explicit_bin: str | None) -> Path:
    if explicit_bin:
        firmware = Path(explicit_bin).expanduser()
    else:
        candidates = sorted(Path(build_dir).glob("*.bin"))
        if not candidates:
            raise FileNotFoundError(f"No .bin found in {build_dir}. Pass --bin explicitly.")
        app_images = [p for p in candidates if p.name not in ("bootloader.bin", "partition-table.bin")]
        firmware = app_images[0] if app_images else candidates[0]

    if not firmware.is_file():
        raise FileNotFoundError(f"Firmware image does not exist: {firmware}")
    return firmware


def parse_key(key_text: str) -> bytes:
    key_text = key_text.strip()
    if len(key_text) != 64:
        raise ValueError("OTA push key must contain exactly 64 hexadecimal digits (AES-256).")
    try:
        key = bytes.fromhex(key_text)
    except ValueError as exc:
        raise ValueError("OTA push key must contain only hexadecimal digits.") from exc
    if len(key) != 32:
        raise ValueError("OTA push key must decode to exactly 32 bytes.")
    return key


def send_push(firmware: Path, host: str, port: int, key_text: str, timeout: float) -> bool:
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError as exc:
        raise RuntimeError("Push mode requires 'cryptography'. Install it with: python -m pip install cryptography") from exc

    key = parse_key(key_text)
    image_size = firmware.stat().st_size
    if image_size <= 0:
        raise ValueError("Firmware image is empty.")
    if image_size > MAX_IMAGE_SIZE:
        raise ValueError(
            f"Firmware is {image_size} bytes, exceeding the ESP push OTA limit of {MAX_IMAGE_SIZE} bytes."
        )

    image = firmware.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    nonce = os.urandom(12)
    # NIXO v1: magic (4), version (1), nonce (12), image length (4, big-endian).
    header = b"NIXO" + b"\x01" + nonce + len(image).to_bytes(4, "big")
    assert len(header) == HEADER_SIZE

    # cryptography's AESGCM returns ciphertext followed by the 16-byte GCM tag.
    encrypted = AESGCM(key).encrypt(nonce, image, header)
    ciphertext, tag = encrypted[:-TAG_SIZE], encrypted[-TAG_SIZE:]
    if len(ciphertext) != image_size:
        raise RuntimeError("Unexpected AES-GCM ciphertext length.")

    LOG.info("Firmware: %s (%d bytes, sha256 %s…)", firmware, image_size, digest[:12])
    LOG.info("Connecting to ESP at %s:%d", host, port)
    LOG.info("Sending authenticated encrypted image (AES-256-GCM)…")

    if trange:
        progress = trange(image_size, unit="B", unit_scale=True, desc="Sending", leave=False)
    else:
        progress = None

    with socket.create_connection((host, port), timeout=timeout) as sock:
        try:
            sock.settimeout(timeout)
            sock.sendall(header)
            sent = 0
            while sent < len(ciphertext):
                end = min(sent + CHUNK_SIZE, len(ciphertext))
                sock.sendall(ciphertext[sent:end])
                if progress:
                    progress.update(end - sent)
                else:
                    pct = int(sent * 100 / image_size)
                    if pct % 10 == 0:
                        LOG.info("Transferred %d%%", pct)
                sent = end
            sock.sendall(tag)
            # We have sent the exact framed request. Half-close to make protocol end explicit,
            # while keeping the receive side open for the ESP response.
            sock.shutdown(socket.SHUT_WR)
        except ConnectionResetError:
            pass
        finally:
            if progress:
                progress.close()
        response = sock.makefile("rb").readline(128).decode("utf-8", errors="replace").strip()

    if response == "OK rebooting":
        LOG.info("ESP accepted and authenticated the image; it is rebooting.")
        return True
    if response:
        raise RuntimeError(f"ESP rejected OTA push: {response}")
    raise RuntimeError("ESP closed the connection without an OTA status response.")


def run_push(args):
    configured_port, configured_key, mqtt_uri, mqtt_user, mqtt_password, node_namespace = read_config()
    port = args.port if args.port is not None else configured_port
    key_text = args.key or os.environ.get("NIXIE_OTA_KEY", "") or configured_key

    if not args.device_host:
        broker_host, broker_port, broker_tls = mqtt_connection_settings(args.mqtt_uri or mqtt_uri)
        host, _topic = wait_for_device_ip(
            args.mqtt_host or broker_host, args.mqtt_port or broker_port,
            broker_tls if args.mqtt_tls is None else args.mqtt_tls,
            args.mqtt_user if args.mqtt_user is not None else mqtt_user,
            args.mqtt_pass if args.mqtt_pass is not None else mqtt_password,
            args.node_prefix or node_namespace, args.ip_timeout,
        )
        args.device_host = host
    if not 1 <= port <= 65535:
        raise ValueError(f"Invalid OTA push TCP port: {port}")

    firmware = select_firmware(args.build_dir, args.bin)
    return send_push(firmware, args.device_host, port, key_text, args.timeout)


def main():
    configured_port, configured_key, mqtt_uri, mqtt_user, mqtt_password, node_namespace = read_config()
    parser = argparse.ArgumentParser(
        description="Send an ESP-IDF application image to an ESP using authenticated OTA push.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Examples:\n"
                "  python3 utils/ota.py # discover IP via MQTT\n"
                "  python3 utils/ota.py --bin build/NixieClock.bin \n"
                "  python3 utils/ota.py --device 192.168.30.42 --bin build/NixieClock.bin\n"
                "  python3 utils/ota.py --device 192.168.30.42 --key <64-hex-digits>\n"
    )
    parser.add_argument("--build-dir", default="build", help="IDF build directory (default: build)")
    parser.add_argument("--bin", default=None, help="Explicit path to application firmware .bin")
    parser.add_argument(
        "-d", "--device-host", "--device", dest="device_host", default=None,
        help="ESP IP address or hostname (default: discover it via MQTT)",
    )
    parser.add_argument(
        "-p", "--port", type=int, default=configured_port,
        help=f"ESP OTA push TCP port (default from sdkconfig: {configured_port})",
    )
    parser.add_argument(
        "-k", "--key", default=os.environ.get("NIXIE_OTA_KEY", "") or configured_key,
        help="64-hex-digit shared AES-256 key (default: NIXIE_OTA_KEY or sdkconfig)",
    )
    parser.add_argument("--timeout", type=float, default=30.0, help="TCP connect/read/write timeout in seconds")
    parser.add_argument("--ip-timeout", type=float, default=20.0, help="Seconds to wait for the device IP via MQTT")
    parser.add_argument("-n", "--node-prefix", default=node_namespace, help="MQTT node prefix used by the device")
    parser.add_argument("-m", "--mqtt-uri", default=mqtt_uri, help="MQTT broker URI from sdkconfig (e.g. mqtts://broker:8883)")
    parser.add_argument("--mqtt-host", default=None, help="Override MQTT broker host")
    parser.add_argument("--mqtt-port", type=int, default=None, help="Override MQTT broker port")
    parser.add_argument("--mqtt-user", default=None, help="Override MQTT username")
    parser.add_argument("--mqtt-pass", default=None, help="Override MQTT password")
    tls = parser.add_mutually_exclusive_group()
    tls.add_argument("--mqtt-tls", dest="mqtt_tls", action="store_true", help="Force MQTT TLS")
    tls.add_argument("--mqtt-no-tls", dest="mqtt_tls", action="store_false", help="Disable MQTT TLS")
    parser.set_defaults(mqtt_tls=None)
    parser.add_argument(
        "-l", "--log-level", default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"], help="Logging level",
    )
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level), format="[%(name)s] %(levelname)s: %(message)s")

    try:
        run_push(args)
    except (OSError, RuntimeError, ValueError) as exc:
        LOG.error("%s", exc)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        LOG.info("Interrupted by user.")
        raise SystemExit(130)
