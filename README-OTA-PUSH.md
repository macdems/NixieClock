# Authenticated OTA push protocol

Configuration in `idf.py menuconfig` -> **OTA Configuration**:

- `OTA_PUSH_PORT`: TCP port; `0` disables push OTA (default `3232`).
- `OTA_PUSH_KEY`: 32-byte AES key encoded as exactly 64 hexadecimal digits. Push OTA remains disabled if it is not valid.

Generate a key, for example:

```sh
openssl rand -hex 32
```

## Wire protocol, version 1

All integer fields are big-endian. The sender connects to the configured TCP port and writes:

| Field             | Size | Description                                              |
|-------------------|-----:|----------------------------------------------------------|
| Magic             |    4 | ASCII `NIXO`                                             |
| Version           |    1 | `0x01`                                                   |
| Nonce             |   12 | Unique random AES-GCM nonce for this key                 |
| Ciphertext length |    4 | Encrypted image size in bytes                            |
| Ciphertext        |    N | AES-256-GCM ciphertext of the raw ESP application `.bin` |
| Tag               |   16 | AES-GCM authentication tag                               |

The complete 21-byte header is GCM additional authenticated data (AAD). Encrypt/decrypt with AES-256-GCM; the key is the 32 bytes represented by `OTA_PUSH_KEY`. Send the response line `OK rebooting` indicates that image validation and boot-partition selection succeeded. Error responses start with `ERR`.

The implementation decrypts and writes the image incrementally to the inactive OTA slot, then authenticates the final GCM tag before selecting that slot for boot. If authentication fails, the candidate slot is not activated. The maximum image size is currently 2 MiB. The client must use a unique nonce for each update and never reuse a nonce with the same key.

This is a custom TCP protocol, not TLS. AES-GCM provides confidentiality and integrity of the firmware payload, but does not prevent replay of a previously valid firmware image. Restrict access to the TCP port with network firewall rules and use secure boot / signed firmware where appropriate.

## Sender script

The project already has `utils/post_ota.py` for pull OTA. The companion
`utils/push_ota.py` selects the transfer method without changing the existing
pull implementation:

```sh
# Existing HTTPS/MQTT pull mode (default)
python utils/push_ota.py --method pull --help

# Send directly to the ESP using AES-256-GCM
python utils/push_ota.py --method push --device-host 192.168.30.42 --bin build/NixieClock.bin
```

Push mode reads `CONFIG_OTA_PUSH_PORT` and `CONFIG_OTA_PUSH_KEY` from
`sdkconfig` when available. The key can instead be supplied through
`--push-key` or the `NIXIE_OTA_PUSH_KEY` environment variable. Avoid putting
long-lived secrets in shell history; the environment variable is preferable.


## ESP-IDF 6.0 / Mbed TLS 4 compatibility

ESP-IDF 6.0 uses Mbed TLS 4.0, where the legacy `mbedtls/gcm.h` crypto API is no longer exposed. The push receiver therefore uses the PSA Crypto multipart AEAD API (`psa/crypto.h`) for AES-256-GCM. `main/CMakeLists.txt` now declares `mbedtls` in `PRIV_REQUIRES`, which supplies the PSA Crypto headers and link dependency.

This change addresses the missing-header issue; the modified firmware still needs to be compiled with the project's exact ESP-IDF checkout and tested on an ESP32-C3 before deployment.

## Automatic device IP discovery over MQTT

The firmware adds a retained `ip/state` message under the device's normal MQTT node ID, e.g. `nixie-clock-001/ip/state`. It publishes the current IPv4 address on MQTT connect and alongside the periodic RSSI update (once per minute). This is a plain MQTT topic, not a Home Assistant entity.

After applying the patch to `main/mqtt.c`, push mode can discover the IP automatically:

```bash
python utils/push_ota.py --method push --bin build/NixieClock.bin
```

The script reads the MQTT broker URI, credentials and node namespace from `sdkconfig`. If needed, use `--mqtt-host`, `--mqtt-port`, `--mqtt-user`, `--mqtt-pass`, `--mqtt-tls`/`--mqtt-no-tls`, and `--node-prefix` to override them. `--device-host` still allows bypassing MQTT discovery. The discovery topic uses the node namespace; where multiple devices share that namespace, specify a node prefix that uniquely identifies the intended device.
