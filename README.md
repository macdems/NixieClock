# IN-14 Nixie Clock

A connected **IN-14 Nixie tube clock** combining classic display technology with modern embedded control.  
This repository contains the complete project: **firmware**, **electrical schematic**, **PCB design**, and **case design**.

## Overview

The project is built around an ESP-based controller and drives IN-14 Nixie tubes to display time with network-connected features such as:

- **Automatic time synchronization**
- **MQTT integration**
- **OTA firmware updates**
- **Persistent settings storage**

![IN-14 Nixie Clock](assets/nixie-clock.gif)

## Repository Contents

- **Firmware** — ESP-IDF project for clock control, MQTT, OTA, and LEDs
- **Schematic** — electrical design of the clock hardware
- **PCB** — board layout and manufacturing files
- **Case design** — enclosure files for the finished device

## Firmware Features

- IN-14 time display control
- Wi-Fi connectivity
- SNTP time synchronization
- MQTT status and command topics
- Home Assistant discovery support
- OTA firmware update over HTTPS
- LED effects and persisted LED state

## Hardware

This project is designed for **IN-14 Nixie tubes** and includes the supporting electronics required for:

- high-voltage tube driving
- logic/control circuitry
- power distribution
- decorative or ambient LED lighting

## Build

Firmware is based on **ESP-IDF**.

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

Configuration can be adjusted with:

```bash
idf.py menuconfig
```

## Project Structure

```text
main/               ESP-IDF application sources
PCB/                PCB layout and fabrication files
NixieClock.FCStd    FreeCAD case design file
NixieClock.3mf      Case design export for 3D printing
```

## Notes

Nixie clocks operate with **high voltage**. Hardware assembly, probing, and testing should be performed carefully and only with appropriate experience.

## License

This project is licensed under the **Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)** license.  
See the [`LICENSE.md`](LICENSE.md) file for full license text.

## Status

This repository is intended as a complete source package for building an IN-14 Nixie clock from electronics to enclosure.

---
A compact Nixie clock project with both vintage character and modern connectivity.
