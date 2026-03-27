# Analog Clock Driver — ESP32-S3 (ESP-IDF v5.4.1)

A stepper-motor-driven analog clock with SNTP time sync, optical position sensing, automatic drift correction, WS2812B LED strips, a rotary-encoder menu, SSD1306 OLED display, OTA firmware updates, and optional Matter (Thread/BLE) smart-home integration.

---

## Hardware

| Signal           | ESP32-S3 GPIO | Notes                                    |
|------------------|---------------|------------------------------------------|
| Stepper IN1      | GPIO 16       | ULN2003 → 28BYJ-48                       |
| Stepper IN2      | GPIO 15       |                                          |
| Stepper IN3      | GPIO 7        |                                          |
| Stepper IN4      | GPIO 6        |                                          |
| LED (sensor)     | GPIO 13       | 330 Ω series resistor                    |
| LDR (ADC in)     | GPIO 14       | 10 kΩ pull-down; ADC2_CH3 (WiFi-safe)   |
| I²C SDA          | GPIO 8        | Shared: SSD1306 display + Seesaw encoder |
| I²C SCL          | GPIO 9        |                                          |
| SSD1306 OLED     | I²C 0x3C      | 128×64, 8×8 font                         |
| Seesaw encoder   | I²C 0x36      | Adafruit Seesaw SAMD09                   |
| LED Strip Ring   | GPIO (RMT)    | WS2812B — default 24 LEDs                |
| LED Strip Base   | GPIO (RMT)    | WS2812B — default 6 LEDs                 |

### Motor notes
- **28BYJ-48** (5 V) driven through **ULN2003** at 3.3 V logic.
- **Half-step mode** (8 phases): 4096 half-steps/revolution.
- Coils **de-energised** after every move — no holding hum or excess heat.
- Default step delay: **2000 µs/step** (~8 s/rev). Adjustable via web UI or UART.

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
| **Clock** | Live time display, SNTP status, timezone |
| **Lights** | Per-strip colour swatches, brightness, effects |
| **Config** | Motor speed, motor direction, LED strip lengths, mDNS hostname, timezone override |
| **Info** | Network status, firmware version, sensor readings, uptime, heap |
| **OTA** | Firmware version check, one-click update, auto-update toggle |
| **Sensor** | Live ADC reading, calibration workflow, diagnostics |

The web UI uses a persistent WebSocket (1-second push) for live updates. Config inputs are only pre-populated on first connect; subsequent pushes only update them if the server value changed externally (e.g. via UART console).

---

## Matter / Smart Home

Exposes **two Extended Color Light endpoints** over Matter:

| Endpoint | Strip |
|----------|-------|
| 1        | Ring  |
| 2        | Base  |

Each endpoint supports On/Off, Level Control (brightness), and Color Control (HS, XY, and color temperature modes). Compatible with Alexa, Apple Home, and Google Home.

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

### Clock & motor
| Command | Description |
|---------|-------------|
| `status` | Full system state dump |
| `set-time [min]` | Move hand to current SNTP minute (or specified minute 0–59) |
| `advance` | Force one test-minute advance |
| `microstep <n> [fwd\|bwd]` | Fine-adjust hand position |
| `set-offset <steps>` | Save sensor-to-12:00 offset (from calibration) |
| `set-step-delay <us>` | Change motor speed (µs/half-step) |
| `motor-reverse [0\|1]` | Flip motor direction |

### Sensor calibration
| Command | Description |
|---------|-------------|
| `calibrate` | Measure dark baseline, set threshold |
| `measure` | Print average ADC reading |
| `cal-scan` | Full auto-calibration: scan for slot, save offset |

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

## Sensor Calibration Workflow

1. Ensure the minute hand is **not** over the sensor.
2. `clock> calibrate` — sets threshold from dark baseline.
3. `clock> cal-scan` — motor sweeps through one revolution, finds the optical slot, saves offset to NVS.
4. Verify: `clock> status` — shows `sensor_offset_steps` (non-zero = calibrated).

---

## Component Overview

| Component | Purpose |
|-----------|---------|
| `stepper_motor` | 28BYJ-48 half-step driver — synchronous, auto power-off |
| `clock_manager` | Motor + sensor + FreeRTOS tick task (1/min, wall-aligned) |
| `position_sensor` | LED/LDR ADC optical sensor |
| `display` | SSD1306 OLED; owns I²C bus handle + mutex |
| `encoder` | Adafruit Seesaw SAMD09 rotary encoder + buttons |
| `menu` | Hierarchical OLED menu; 5-minute display blank timeout |
| `networking` | WiFi STA, SNTP, IP geolocation, mDNS |
| `webserver` | HTTP + WebSocket server; serves single-page web UI |
| `led` | WS2812B strip driver (RMT); effects: static, breathe, rainbow, chase, sparkle, comet, wipe |
| `matter` | Matter bridge — two Extended Color Light endpoints |
| `ota_manager` | Background OTA task; polls GitHub version.json |
| `config_store` | NVS persistence for clock, LED, and network config |

---

## Configuration Constants

| File | Constant | Default | Purpose |
|------|----------|---------|---------|
| `stepper_motor.h` | `DEFAULT_STEP_DELAY_US` | 2000 | µs between half-steps |
| `stepper_motor.h` | `MOTOR_REVS_PER_CLOCK_MINUTE` | 1.0 | Gear ratio |
| `clock_manager.h` | `SENSOR_WINDOW_SECONDS` | 30 | ±s around hour to check sensor |
| `clock_manager.h` | `MAX_AUTO_CORRECT_MINUTES` | 5 | Max automatic drift correction |
| `main/main.cpp` | `WIFI_SSID_DEFAULT` / `WIFI_PASSWORD_DEFAULT` | `""` | Fallback credentials (empty = use NVS) |
| `main/main.cpp` | `TZ_OVERRIDE` | `""` | POSIX TZ string; empty = use geolocation |

---

## Noise Reduction Tips

1. **Increase `DEFAULT_STEP_DELAY_US`** to 2500–3000 for quieter operation.
2. **Coil power-off** is automatic after every move — no holding hum.
3. **Mount the motor on foam** or rubber grommets to isolate vibration.
4. **Half-step mode** (already used) is inherently quieter than full-step.
