# PhotonFrame

An indoor solar-powered e-paper photo frame with intelligent power management.

## Overview

PhotonFrame is a FireBeetle ESP32-E powered 7.3" Spectra 6 (7-color) e-paper display that:
- Fetches images from Nextcloud WebDAV
- Monitors solar panel power via INA228 sensor
- Adjusts refresh intervals based on available power + battery voltage
- Reports telemetry to Home Assistant via MQTT
- Supports OTA updates from Nextcloud and GitHub

## Hardware Requirements

| Component | Model | Notes |
|-----------|-------|-------|
| Microcontroller | DFRobot FireBeetle ESP32-E | Any ESP32 with sufficient GPIO |
| Display | Good Display GDEP073E01 | 7.3" 800x480 Spectra 6 (7-color) |
| Power Sensor | INA228 module | Adafruit or generic, 15mΩ shunt |
| Solar Panel | 5V indoor panel | ~100-200mW for indoor light |
| Battery | 3.7V LiPo | 1000-2000mAh recommended |

## Wiring

### Pinout Summary

```
FireBeetle ESP32-E Pin Assignments
==================================

I2C (INA228 Power Sensor)
  GPIO 21 ─────── SDA
  GPIO 22 ─────── SCL
  GPIO 13 ─────── INA228 VCC (switched power)

SPI (Spectra 6 Display)
  GPIO 18 ─────── SCK (Clock)
  GPIO 23 ─────── MOSI (DIN)
  GPIO 19 ─────── MISO (unused)
  GPIO  5 ─────── CS (Chip Select)
  GPIO 17 ─────── DC (Data/Command)
  GPIO 16 ─────── RST (Reset)
  GPIO  4 ─────── BUSY

Power Monitoring
  GPIO 34 ─────── Battery voltage (built-in ADC divider)

Status
  GPIO  2 ─────── Built-in LED
```

### Wiring Diagram

```
                    ┌─────────────────────────────────────┐
                    │        FireBeetle ESP32-E           │
                    │                                     │
   INA228           │                                     │    Spectra 6 Display
┌──────────┐        │                                     │    ┌──────────────┐
│ VCC ─────┼────────┤ GPIO 13                             │    │              │
│ GND ─────┼────────┤ GND                         GPIO 18 ├────┤ CLK          │
│ SDA ─────┼────────┤ GPIO 21                     GPIO 23 ├────┤ DIN (MOSI)   │
│ SCL ─────┼────────┤ GPIO 22                      GPIO 5 ├────┤ CS           │
│          │        │                             GPIO 17 ├────┤ DC           │
│ VIN+ ────┼─ Solar+│                             GPIO 16 ├────┤ RST          │
│ VIN- ────┼─ Solar-│                              GPIO 4 ├────┤ BUSY         │
└──────────┘        │                                3.3V ├────┤ VCC          │
                    │                                 GND ├────┤ GND          │
   Solar Panel      │                                     │    └──────────────┘
┌──────────┐        │                                     │
│ + ───────┼─ VIN+ on INA228                              │
│ - ───────┼─ VIN- on INA228 ─── Battery charge circuit   │
└──────────┘        │                                     │
                    └─────────────────────────────────────┘
```

### INA228 Connection Details

The INA228 measures current through its shunt resistor. Connect:
- **VIN+**: Solar panel positive
- **VIN-**: To battery charging circuit (through shunt)
- **VCC**: Connected to GPIO 13 for switchable power
- **GND**: Common ground
- **SDA/SCL**: I2C bus

## Software Setup

### 1. Install PlatformIO

Install [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) for VS Code.

### 2. Clone/Download Project

```bash
cd ~/Documents/PlatformIO/Projects
git clone https://github.com/YourUsername/PhotonFrame.git
cd PhotonFrame
```

### 3. Configure Secrets

Copy the example secrets file and edit with your credentials:

```bash
cp secrets.example.h secrets.h
```

Edit `secrets.h` with:
- WiFi credentials
- MQTT broker details
- Nextcloud WebDAV URL and credentials

### 4. Build and Upload

```bash
pio run -t upload
pio device monitor
```

## Intelligent Refresh Algorithm

PhotonFrame uses a **hybrid power-aware** refresh algorithm that considers both battery voltage AND real-time solar power:

| Battery | Solar Power | Refresh Interval | Power State |
|---------|-------------|------------------|-------------|
| ≥3.9V   | >100µW      | 30 minutes       | `abundant`  |
| ≥3.9V   | <100µW      | 2 hours          | `neutral`   |
| 3.6-3.9V| >100µW      | 1 hour           | `neutral`   |
| 3.6-3.9V| <100µW      | 4 hours          | `conserving`|
| 3.4-3.6V| >50µW       | 4 hours          | `conserving`|
| 3.4-3.6V| <50µW       | 8 hours          | `low`       |
| <3.4V   | any         | 24 hours         | `emergency` |

### Power Budget

- **Display refresh**: ~0.4mAh per cycle (15-20s @ 80mA)
- **WiFi + MQTT**: ~0.1mAh per wake cycle
- **Deep sleep**: ~10µA
- **Indoor solar**: ~50-200µW typical

With indoor solar producing ~50µA average, one refresh cycle needs ~8 hours to recover!

## MQTT Integration

### Topics

| Topic | Type | Description |
|-------|------|-------------|
| `photonframe/state` | JSON | All sensor data (retained) |
| `photonframe/availability` | String | `online`/`offline` (LWT) |
| `photonframe/command` | JSON | Commands to device |
| `photonframe/status` | String | Current status message |

### State Payload

```json
{
  "battery_percentage": 85,
  "battery_voltage": 3.92,
  "solar_voltage_mV": 450.2,
  "solar_current_uA": 125.3,
  "solar_power_uW": 56.4,
  "ina_temperature": 24.5,
  "power_state": "neutral",
  "next_refresh_seconds": 7200,
  "firmware_version": "1.0.0",
  "wifi_rssi": -52,
  "free_heap": 180000,
  "boot_count": 42,
  "successful_updates": 38,
  "error_count": 0,
  "last_error": "none",
  "ip_address": "192.168.1.123"
}
```

### Commands

Send JSON to `photonframe/command`:

```json
{"action": "update_display"}     // Force image refresh
{"action": "reboot"}             // Restart device
{"action": "check_ota"}          // Check for firmware update
{"action": "get_status"}         // Force state publish
{"action": "clear_errors"}       // Reset error counters
{"action": "force_ha_discovery"} // Republish HA discovery configs
```

### Home Assistant Auto-Discovery

PhotonFrame automatically registers sensors with Home Assistant via MQTT discovery. Sensors appear under a single device with:
- Battery percentage & voltage
- Solar voltage, current, power
- INA228 temperature
- Power state
- WiFi signal strength
- Diagnostic sensors (boot count, errors, etc.)

## OTA Updates

### Nextcloud OTA (Priority)

1. Build firmware: `pio run`
2. Upload `.pio/build/firebeetle32/firmware.bin` to Nextcloud:
   `/Shared/firmware/photonframe/firmware.bin`
3. Device checks on each boot and auto-updates
4. File is deleted after successful update

### GitHub OTA (Fallback)

1. Create a GitHub release with tag `v1.x.x`
2. Attach `firmware.bin` to the release
3. Device checks GitHub if Nextcloud has no update

## Nextcloud Image Setup

### Image Requirements

- **Format**: PNG (8-bit RGB)
- **Size**: 800x480 or will be scaled
- **Colors**: Will be quantized to 7-color palette

### Recommended Nextcloud Folder Structure

```
/Photos/EPaper/
  ├── display.png      (primary image)
  └── fallback.png     (backup image)

/Shared/firmware/photonframe/
  └── firmware.bin     (OTA updates)
```

### Automatic Image Generation

Consider using a Nextcloud Flow or external script to:
1. Generate daily images (calendar, weather, photos)
2. Resize/optimize for e-paper
3. Save to the configured folder

## Troubleshooting

### Display Not Updating

1. Check SPI wiring (especially BUSY pin)
2. Verify display power (3.3V, not 5V!)
3. Check serial output for PNG decode errors
4. Ensure image is valid PNG, not too large (<500KB)

### WiFi Connection Issues

1. Device creates `PhotonFrame-Setup` AP after failed connections
2. Connect to AP, configure WiFi via captive portal
3. Check serial output for connection attempts

### INA228 Not Detected

1. Verify I2C wiring (SDA/SCL not swapped)
2. Check GPIO 13 is providing power to INA228
3. Default I2C address is 0x40 (A0=GND, A1=GND)
4. Try I2C scanner sketch to verify address

### MQTT Not Publishing

1. Verify broker IP/port in secrets.h
2. Check MQTT username/password
3. Ensure broker allows connections from device IP
4. Check for firewall blocking port 1883

### Low Battery Life

1. Ensure INA228 is powered off during sleep (GPIO 13 LOW)
2. Check display is in hibernate mode
3. Verify deep sleep current (~10µA expected)
4. Indoor solar may need larger panel for your lighting

## Configuration Reference

### config.h Constants

| Constant | Default | Description |
|----------|---------|-------------|
| `INA228_ADDR` | 0x40 | I2C address |
| `SHUNT_RESISTOR` | 0.015 | 15mΩ shunt value |
| `VOLTAGE_CRITICAL` | 3.4V | Emergency sleep threshold |
| `VOLTAGE_LOW` | 3.6V | Low battery threshold |
| `VOLTAGE_FULL` | 3.9V | Full battery threshold |
| `POWER_ABUNDANT_UW` | 100 | µW threshold for "abundant" |
| `POWER_LOW_UW` | 50 | µW threshold for "low" |
| `SLEEP_MIN_SECONDS` | 1800 | 30 min minimum refresh |
| `SLEEP_MAX_SECONDS` | 86400 | 24 hour maximum refresh |

### secrets.h Required Values

| Secret | Description |
|--------|-------------|
| `SECRET_WIFI_SSID` | WiFi network name |
| `SECRET_WIFI_PASSWORD` | WiFi password |
| `SECRET_MQTT_BROKER` | MQTT broker IP/hostname |
| `SECRET_MQTT_PORT` | MQTT port (usually 1883) |
| `SECRET_MQTT_USERNAME` | MQTT username (or empty) |
| `SECRET_MQTT_PASSWORD` | MQTT password (or empty) |
| `SECRET_MQTT_CLIENT_ID` | Unique MQTT client ID |
| `SECRET_NEXTCLOUD_URL` | WebDAV URL to image folder |
| `SECRET_NEXTCLOUD_USER` | Nextcloud username |
| `SECRET_NEXTCLOUD_PASS` | Nextcloud app password |
| `SECRET_NEXTCLOUD_PHOTO` | Primary image filename |
| `SECRET_NEXTCLOUD_FALLBACK` | Fallback image filename |

## Project Structure

```
PhotonFrame/
├── platformio.ini          # Build configuration
├── secrets.h               # Your credentials (gitignored)
├── secrets.example.h       # Template for secrets
├── README.md               # This file
├── src/
│   ├── main.cpp            # Main firmware
│   ├── config.h            # Pin definitions, constants
│   ├── display.h           # Display interface
│   ├── display.cpp         # Spectra 6 driver
│   ├── power.h             # Power monitoring interface
│   ├── power.cpp           # INA228 driver + sleep logic
│   └── ota.h               # OTA update classes
└── docs/
    └── PINOUT.md           # Detailed wiring reference
```

## License

MIT License - See LICENSE file for details.

## Credits

Built on code from:
- [INA228-Power-Monitor](https://github.com/...) - INA228 driver
- [Paperlesspaper7](https://github.com/Dreadmond/paperlesspaper7-enhanced) - Display & MQTT code
- [GxEPD2](https://github.com/ZinggJM/GxEPD2) - E-paper display library
- [pngle](https://github.com/kikuchan/pngle) - PNG decoder
