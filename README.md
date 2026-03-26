# BP Checker ESP8266

ESP8266-based WiFi blood pressure monitor bridge. Reads data from OMRON blood pressure monitors via serial (USB) and serves results over a web interface.

## Features

- AP mode with configurable WiFi connection
- mDNS support (`bp_checker.local`)
- Web server for reading blood pressure data
- Supports OMRON HBP-9030 (extensible)

## Hardware

- ESP8266 (e.g., NodeMCU, Wemos D1 Mini)
- USB serial connection to blood pressure monitor

## Setup

1. Flash firmware via Arduino IDE or PlatformIO
2. Connect to `ESP8266_BP_checker` AP
3. Configure WiFi via web interface

## License

See repository owner for license information.
