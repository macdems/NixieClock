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

### Bill of Materials (BOM)

| Ref                                | Qty | Value               | Description                                                                       |
| ---------------------------------- | --- | ------------------- | --------------------------------------------------------------------------------- |
| C1                                 | 1   | 220µF/35V           | electrolytic capacitor THT radial D5.0mm P2.50mm                                  |
| C2                                 | 1   | 220µF               | electrolytic capacitor THT radial D5.0mm P2.50mm                                  |
| D1, D2, D3, D4                     | 4   | WS2812B             | WS2812B-1209 SMD mount LED                                                        |
| D5, D7                             | 2   | SS24                | SMA package                                                                       |
| D6                                 | 1   | ES1G                | SMA package                                                                       |
| HS1                                | 1   | T8WC                | heatsink for for TO-220 (<https://aliexpress.com/item/1005006082544471.html>)     |
| J1                                 | 1   | 12V                 | 01x02 screw terminal P5.08mm                                                      |
| J2                                 | 1   | 180V                | 01x02 screw terminal P5.08mm                                                      |
| L1                                 | 1   | 150mH               | SMD inductor L_10.4x10.4 H4.8                                                     |
| N1, N2, N3, N4                     | 4   | IN-14               | Nixie IN-14                                                                       |
| N5                                 | 1   |                     | 02×02 pin socket P2.54mm                                                          |
| N5                                 | 1   |                     | Nixie colon separator (<https://aliexpress.com/item/1005009385409998.html>)       |
| Q1, Q2, Q3, Q4, Q5, Q6, Q7, Q8, Q9 | 9   | MMBTA44             | SOT-23 package                                                                    |
| R1, R2, R3, R4                     | 4   | 12k                 | 1026 SMD                                                                          |
| R5, R6                             | 2   | 390k                | 1026 SMD                                                                          |
| U1, U2, U3, U4                     | 4   | SN74141N            | DIP-16 W7.62mm                                                                    |
| U5, U6, U7                         | 3   | 74HC595             | DIP-16 W7.62mm                                                                    |
| U8                                 | 1   | LM2596T-5           | TO-220-5                                                                          |
| U9                                 | 1   | ESP32-C3 Super Mini |                                                                                   |
|                                    | 1   | NCH6100HV           | high voltage DC power supply for Nixie tubes                                      |
|                                    | 6   | M3×5×4              | brass insert M3 H5mm OD4mm                                                        |
|                                    | 2   | M2.5×3×3.5          | brass insert M2.5 H3mm OD3.5mm                                                    |
|                                    | 1   | DC022B              | DC power jack supply socket (<https://aliexpress.com/item/1005005583190148.html>) |

### Manufacturing

1. Order the PCB from a manufacturer such as **JLCPCB** or **PCBWay**using the provided KICAD project files.
2. Assemble the components according to the schematic and PCB layout.
3. 3D print or fabricate the case using the provided FreeCAD design files.
4. Paint the case if desired and allow to dry before final assembly.
5. Mount the DC power jack and brass inserts into the case, then secure the PCB inside the case using screws and inserts.
6. Solder wires to the DC power jack and connect them to both 12V input of the PCB and the NCH6100HV power supply input. Ensure proper polarity and secure connections. 
7. Connect the NCH6100HV 180V output to the J2 high voltage input on the PCB, ensuring secure and insulated connections to prevent accidental contact with high voltage.

## Build

Firmware is based on **ESP-IDF**. First you need to configure it, specyfying your WiFi and MQTT creedentials. Then you can build and flash the firmware with:

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
