This repository contains an ESP-IDF C project implementing a Nixie tube clock with MQTT, OTA, and LED control.

Keep instructions concise and actionable for an AI coding agent working on this codebase. Focus on discoverable patterns and concrete file locations.

Quick architecture summary
- Runtime: ESP-IDF (FreeRTOS). Entry point: `main/app_main()` in `main/main.c`.
- Components:
  - WiFi + SNTP: `main/clock.c` initializes time (SNTP) and spawns the clock task.
  - MQTT integration: `main/mqtt.c` builds topics as `{NODE_ID}/{topic...}` using MAC-based `NODE_ID`. Discovery payloads for Home Assistant are constructed here.
  - LED strip: `main/leds.c` uses `led_strip` SPI backend and NVS for persisted state.
  - OTA update: `main/ota.c` performs HTTPS OTA and publishes progress to MQTT.
  - GPIO helpers: `main/gpio.c` small wrapper used across modules.

Key conventions and patterns (do not change without reason)
- Topic namespace: all MQTT topics are published with a prefix built from `CONFIG_NODE_NAMESPACE` + MAC address in `init_mqtt_client()` (`main/mqtt.c`). Use `mqtt_client_publish(topic, payload, retain)` to publish — the function prefixes the topic.
- Discovery: MQTT discovery messages are built in `publish_*_discovery()` functions inside `main/mqtt.c`. If adding new devices, follow the same JSON structure and call `finish_discovery_payload()`.
- Persistent state: use NVS (see `init_leds()` in `main/leds.c`) — keys: `leds_on`, `leds_hue`, `leds_saturation`, `leds_value`.
- OTA flow: OTA is started by publishing a URL to `firmware/set` topic. The code spawns `ota_task` which reports to `firmware/status` and `firmware/progress`.
- Time handling: timezone is set explicitly to CET/CEST in `init_clock()`; SNTP servers are configured inline.

Build & developer workflows
- Build, flash, monitor (standard esp-idf): use `idf.py -p <PORT> build flash monitor` from repo root (the project uses CMake / ESP-IDF). Configuration options come from `menuconfig` (`idf.py menuconfig`).
- If NVS init fails due to partition mismatch, the code already erases NVS and retries in `app_main()`.
- To run unit or CI tests: there are no project tests in repo; prefer small local run on hardware. For logs, use `idf.py -p /dev/ttyUSB0 monitor`.

Files worth reading when making changes
- `main/main.c` — boot sequence, wifi event handlers, where mqtt/clock/leds are started.
- `main/mqtt.c` — topic naming, discovery payloads, incoming command parsing. Central place for adding new MQTT-controlled features.
- `main/clock.c` — SNTP, timezone, clock task; important for timing-sensitive display updates.
- `main/leds.c` — led_strip backend, HSV handling, NVS persistence and how state is echoed via MQTT.
- `main/ota.c` — OTA state machine and MQTT progress reporting.

Small code patterns to follow
- Use existing helper functions: `configure_gpio(pin, level)` from `main/gpio.c` and `mqtt_client_publish()` to ensure consistent topic prefixing.
- Use ESP logging macros (`ESP_LOGI/W/E/D`) with file-local TAG constants (already present in modules).
- When adding new persistent keys, use a dedicated NVS namespace (see `nvs_open("leds", ...)`).

Edge cases and runtime behaviors to be aware of
- `init_mqtt_client()` waits 3s before starting the client to let WiFi stabilize — avoid racing MQTT connect with immediate heavy publish bursts.
- SNTP sync is waited for up to 30s (`esp_netif_sntp_sync_wait`) — avoid blocking longer initialization on network edge cases.
- OTA uses `esp_https_ota` and may return chunked transfers (content-length -1). The code handles progress reporting differently in that case.

If you need more context or to change build configs, look at: `CMakeLists.txt`, `sdkconfig`, and `main/idf_component.yml`.

Use MQTT discovery prefix (`CONFIG_MQTT_DISCOVERY_PREFIX`) in new features. Always add `finish_discovery_payload` for common configuration.

Additional NVS namespaces are allowed and should be contextual.

If anything above is unclear, return this file and ask for targeted clarifications (e.g., desired MQTT topic layout, preferred NVS namespaces, or platform constraints).
