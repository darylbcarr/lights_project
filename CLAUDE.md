# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WS2812B LED controller with SSD1306 OLED + two-button UI, SNTP time sync, OTA firmware updates, and Matter smart-home integration. Target: **ESP32-S3**, framework: **ESP-IDF v5.4.1**, language: **C++17**.

## Build Commands

All commands go through `./build.sh`, which sets up the ESP-IDF environment automatically.

```bash
# Build firmware
./build.sh build

# Flash + open serial monitor
./build.sh -p /dev/ttyUSB0 flash monitor

# Flash pre-built artifacts only (skip CMake/ninja — fast re-flash)
./build.sh -p /dev/ttyUSB0 flash-fast monitor

# Monitor only (no flash)
idf.py -p /dev/ttyUSB0 monitor

# Interactive Kconfig editor
./build.sh menuconfig
```

**Serial console:** 115200 baud, prompt `clock> ` — UART commands for diagnostics, Matter, and OTA.

## Releases / OTA

- Bump `PROJECT_VER` in `CMakeLists.txt` and `version.json` before tagging.
- GitHub Actions (CI) builds on version tags and uploads the `.bin` as a release asset.
- Devices poll `version.json` every 24 hours; 60 KB free-heap guard prevents OTA during BLE commissioning.

## Architecture

### Boot & Setup Flow (`main/main.cpp`)
On first boot (no NVS credentials, no Matter fabric) the OLED shows a setup menu:
- **WiFi path** → user enters SSID/password via button UI → web UI starts → `wifi_only=true` persisted (Matter/BLE never starts again).
- **Matter path** → BLE commissioning → Matter provisions WiFi → `matter_commissioned=true` persisted.
- **Factory reset**: hold A+B buttons 2 s → clears NVS flags → returns to setup on next boot.

### I²C Bus
`display` owns the I²C bus handle (GPIO8/9) and a mutex. All I²C traffic (OLED at 0x3C) is serialized through that mutex.

### FreeRTOS Task Topology
| Task | Rate | Owner |
|------|------|-------|
| `button_poll` | 50 Hz | `main` |
| `blank_timer` | 1 Hz (5-min timeout) | `menu` |
| `ws_push_task` | 1 s | `webserver` |
| `ota_task` | 24 h | `ota_manager` |
| `geo_task` | one-shot after WiFi | `networking` |

### Key Component Responsibilities
- **`display`** — SSD1306 via new ESP-IDF I²C driver; owns bus handle + mutex.
- **`menu`** — hierarchical OLED menu driven by button events; 5-min blank timeout.
- **`networking`** — WiFi STA, SNTP, IP geolocation (ip-api.com) → IANA→POSIX TZ, mDNS (hostname defaults to `clock_<last4mac>.local`).
- **`webserver`** — HTTP + WebSocket (1 s push); single-page web UI served from embedded HTML; 6 tabs.
- **`led`** — WS2812B via RMT; Ring (24 LEDs) + Base (6 LEDs) strips; 8 effects (static, breathe, rainbow, chase, sparkle, comet, wipe, off).
- **`matter`** — Matter bridge; 2 Extended Color Light endpoints (Ring=ep1, Base=ep2); HS/XY/CT color modes.
- **`ota_manager`** — polls GitHub `version.json`; `esp_https_ota`; auto-update toggle.
- **`config_store`** — NVS wrapper; `LedCfg`, `NetCfg` structs.

## Hardware Pin Reference
| Signal | GPIO | Notes |
|--------|------|-------|
| Button A | GPIO 0 | Active-low, internal pull-up |
| Button B | GPIO 1 | Active-low, internal pull-up |
| I²C SDA/SCL | 8, 9 | OLED only (0x3C) |
