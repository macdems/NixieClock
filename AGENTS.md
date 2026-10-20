# Repository Guidance

This repository contains an ESP-IDF C firmware project for an IN-14 Nixie clock, plus PCB and enclosure design files. See [README.md](README.md) for the project overview and basic build commands.

## Firmware Layout

- `main/main.c` owns startup and Wi-Fi event handling.
- `main/clock.c` configures SNTP/timezone and runs the clock task; `main/nixies.c` controls the tube display and `main/effects.c` implements display effects.
- `main/mqtt.c` handles MQTT topics, commands, and Home Assistant discovery.
- `main/ota.c` implements HTTPS OTA; `main/nvs.c` owns persistent settings; `main/gpio.c` provides GPIO helpers.
- This is a single `main` component. Add new source files to `main/CMakeLists.txt`.

## Build And Validation

- Build from the repository root with `idf.py build`; configure with `idf.py menuconfig`.
- Flash and monitor a connected device with `idf.py -p <PORT> flash monitor`.
- There is no project test suite or CI workflow. Use the build as the default code check; hardware behavior still needs device validation.
- Check `sdkconfig.defaults`, `sdkconfig`, and `main/idf_component.yml` for target, configuration, and dependency details. Do not assume the local ESP-IDF version or serial port; use the active environment and user-provided port.

## Project Conventions

- Publish application MQTT messages through `mqtt_client_publish(topic, payload, retain)`, which applies the configured namespace. The MAC suffix is optional and controlled by `CONFIG_NODE_ADD_SUFFIX`.
- Add Home Assistant entities in the existing `publish_*_discovery()` flow in `main/mqtt.c`, use `CONFIG_MQTT_DISCOVERY_PREFIX`, and finish payloads with `finish_discovery_payload()`.
- Persistent settings currently use the `nixie-clock` NVS namespace. Follow the existing helpers and keys in `main/nvs.c` (`blink_colon`, `use_vegas`, and `cathode_protect`) rather than assuming LED-strip settings.
- Follow nearby module conventions: file-local `TAG` constants, ESP logging macros, and existing helpers such as `configure_gpio()`.

## Runtime Constraints

- `init_mqtt_client()` delays for three seconds and is called from the got-IP event path. Avoid adding substantial blocking work to Wi-Fi event callbacks.
- SNTP synchronization may wait up to 30 seconds. Keep other startup waits bounded.
- NVS is erased and retried only for `ESP_ERR_NVS_NO_FREE_PAGES`; preserve that limited recovery behavior unless a change is intentional.
- OTA uses push mechanism with the following constraints:
  - Encryption and authentication: AES-256-GCM with a symmetric key configured through Kconfig. The authentication tag must be valid before the update is considered valid.
  - Secure writing: receiving the image in chunks, checking the size and handling interrupted connections. An unverified image cannot be activated.
  - Configuration: separate options for the port and key for push OTA. An empty or invalid key should disable this feature, not cause an insecure server to start.

## Scope

Keep changes focused on the owning module and preserve the established MQTT, NVS, and ESP-IDF patterns. Firmware changes do not imply edits to the PCB or CAD assets. Nixie hardware uses high voltage; do not recommend probing or assembly without appropriate precautions.
