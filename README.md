# WS2812 LED Strip Driver — ESP32-S3 (ESP-IDF v5.4.1)

WS2812B LED strips, SSD1306 OLED display, OTA firmware updates, and optional Matter (Thread/BLE) smart-home integration.

---

## Hardware

| Signal           | ESP32-S3 GPIO | Notes                       |
|------------------|---------------|-----------------------------|
| I²C SDA          | GPIO 8        | Shared: SSD1306 display     |
| I²C SCL          | GPIO 9        |                             |
| SSD1306 OLED     | I²C 0x3C      | 128×64, 8×8 font            |
| LED Strip Left   | GPIO (RMT)    | WS2812B — default 30 LEDs   |
| LED Strip Right  | GPIO (RMT)    | WS2812B — default 30 LEDs   |

---

## Building

Use `build.sh` which wraps all ESP-IDF environment setup:

```bash
# Build
./build.sh build

# Flash and open monitor
./build.sh -p /dev/ttyUSB0 flash monitor

# Menuconfig
./build.sh menuconfig

# Monitor only (no flash)
idf.py -p /dev/ttyUSB0 monitor
```

---

## First-Time Setup

On first boot (no saved WiFi credentials and no Matter fabric), the device shows a setup menu on the OLED:

**WiFi path**
1. Select **WiFi** → enter SSID and password via rotary encoder.
2. Device connects, geolocates timezone, syncs SNTP, and starts the web UI.
3. `wifi_only = true` is persisted — Matter/BLE never starts on subsequent boots.

**Matter path**
1. Select **Matter** → display shows discriminator and pairing code.
2. Pair using Apple Home, Google Home, or Alexa (scan QR or enter code shown on UART).
3. Matter provisions WiFi credentials over BLE; device connects and starts normally.
4. `matter_commissioned = true` is persisted so subsequent reboots skip setup.

**Factory reset**: Hold both **A + B** buttons for 2 seconds on any screen. Clears WiFi credentials, Matter fabric, and the `wifi_only`/`matter_commissioned` flags — device returns to first-time setup on next boot.

---

## Web UI

Accessible at `http://<device-ip>/` or `http://<mdns-hostname>.local/` (default hostname derived from MAC, e.g. `clock_a1b2.local`).

Sections:

| Tab | Contents |
|-----|----------|
| **Lights** | Per-strip colour swatches, brightness, effects |
| **Config** | Motor speed, motor direction, LED strip lengths, mDNS hostname, timezone override |
| **Info** | Network status, firmware version, sensor readings, uptime, heap |
| **OTA** | Firmware version check, one-click update, auto-update toggle |

The web UI uses a persistent WebSocket (1-second push) for live updates. Config inputs are only pre-populated on first connect; subsequent pushes only update them if the server value changed externally (e.g. via UART console).

---

## Matter / Smart Home

Exposes **one Extended Color Light endpoint** over Matter:

The endpoint supports On/Off, Level Control (brightness), and Color Control (HS, XY, and color temperature modes). Compatible with Alexa, Apple Home, and Google Home.

**Re-opening the commissioning window** (e.g. to add a second controller):
`clock> matter-open-window`
or via the web UI Matter menu.

**Colour calibration**: Alexa uses XY color mode for named colours. The web UI colour presets are normalised to max-channel = 255 to match Matter output. To verify exact RGB for any Alexa colour, check the UART monitor — the firmware logs:
`I MatterBridge: ep0 [XY] → RGB(0,255,0) bri=200`

---

## OTA Firmware Updates

The device polls a GitHub `version.json` file for new releases. The check interval and auto-update toggle are configurable via the web UI (Config → OTA). Manual update: web UI OTA tab → **Check Now** → **Install**.

A 60 KB free-heap guard prevents OTA from running when memory is low (e.g. during active BLE commissioning).

---

## UART Console

Connect at **115200 baud**. Prompt: `clock> `

### Network & time
| Command | Description |
|---------|-------------|
| `time [fmt]` | Print current time (optional strftime format) |
| `sntp-status` | WiFi / SNTP sync status |
| `set-tz <posix>` | Override POSIX timezone string |

### Matter
| Command | Description |
|---------|-------------|
| `matter-status` | Commissioning state, fabric count |
| `matter-open-window` | Re-open BLE commissioning window |
| `matter-clear` | Erase Matter fabric data (requires restart) |

### OTA
| Command | Description |
|---------|-------------|
| `ota-check` | Check GitHub for new firmware |
| `ota-update` | Check and install if update available |

---

## Component Overview

| Component | Purpose |
|-----------|---------|
| `display` | SSD1306 OLED; owns I²C bus handle + mutex |
| `encoder` | Adafruit Seesaw SAMD09 rotary encoder + buttons |
| `menu` | Hierarchical OLED menu; 5-minute display blank timeout |
| `networking` | WiFi STA, SNTP, IP geolocation, mDNS |
| `webserver` | HTTP + WebSocket server; serves single-page web UI |
| `led` | WS2812B strip driver (RMT); effects: static, breathe, rainbow, chase, sparkle, comet, wipe |
| `matter` | Matter bridge — two Extended Color Light endpoints |
| `ota_manager` | Background OTA task; polls GitHub version.json |
| `config_store` | NVS persistence for clock, LED, and network config |
