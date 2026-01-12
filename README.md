# PhotonFrame

An indoor solar-powered e-paper photo frame with intelligent power management.

## Overview

PhotonFrame is a FireBeetle ESP32-E powered 7.3" Spectra 6 (7-color) e-paper display that:
- Fetches images from Nextcloud WebDAV
- Monitors solar panel power via INA228 sensor
- Adjusts refresh intervals based on available power + battery voltage
- Reports telemetry to Home Assistant via MQTT
- Supports OTA updates (Arduino OTA, Nextcloud, GitHub)

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
  GPIO  3 ─────── CS (Chip Select)
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
│ SCL ─────┼────────┤ GPIO 22                      GPIO 3 ├────┤ CS           │
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
- **SDA/SCL**: I2C bus (GPIO 21/22)

**I2C Address**: Default 0x40 (A0=GND, A1=GND)

## Software Setup

### 1. Install PlatformIO

Install [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) for VS Code.

### 2. Clone Project

```bash
cd ~/Documents/PlatformIO/Projects
git clone https://github.com/Dreadmond/PhotonFrame.git
cd PhotonFrame
```

### 3. Configure Secrets

```bash
cp secrets.example.h secrets.h
```

Edit `secrets.h` with your credentials:
- WiFi SSID and password
- MQTT broker details
- Nextcloud WebDAV URL and credentials
- Arduino OTA password

### 4. Build and Upload

```bash
pio run -t upload
pio device monitor
```

## Intelligent Refresh Algorithm

PhotonFrame uses a **hybrid power-aware** refresh algorithm:

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

## OTA Updates

### Arduino OTA (Development)

The device listens for Arduino OTA connections for 10 seconds after each boot.

```bash
# Upload via OTA (after initial USB flash)
pio run -t upload --upload-port photonframe.local
```

**Credentials:**
- Hostname: `photonframe.local`
- Password: Set in `secrets.h` (default: `photonframe123`)

To keep the device awake for OTA, send MQTT command:
```json
{"action": "ota_mode"}
```
Device stays awake for 5 minutes with blinking LED.

### Nextcloud OTA (Production)

1. Build: `pio run`
2. Upload `.pio/build/firebeetle32/firmware.bin` to:
   `/Shared/firmware/photonframe/firmware.bin`
3. Device auto-updates on next boot
4. File deleted after successful update

### GitHub OTA (Fallback)

1. Create GitHub release with tag `v1.x.x`
2. Attach `firmware.bin` to release
3. Device checks GitHub if no Nextcloud update

## MQTT Integration

### Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `photonframe/state` | Out | All sensor data (JSON, retained) |
| `photonframe/availability` | Out | `online`/`offline` (LWT) |
| `photonframe/command` | In | Commands (JSON) |
| `photonframe/status` | Out | Status messages |

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

| Command | Description |
|---------|-------------|
| `{"action": "update_display"}` | Force image refresh on next boot |
| `{"action": "reboot"}` | Restart device immediately |
| `{"action": "check_ota"}` | Check Nextcloud/GitHub for updates |
| `{"action": "ota_mode"}` | Stay awake 5 min for Arduino OTA |
| `{"action": "clear_errors"}` | Reset error counters |
| `{"action": "force_ha_discovery"}` | Republish HA discovery configs |

### Home Assistant Auto-Discovery

PhotonFrame registers sensors automatically via MQTT discovery:
- Battery percentage & voltage
- Solar voltage, current, power
- INA228 temperature
- Power state
- Next refresh time
- WiFi signal strength
- Boot count, errors, firmware version

Sensors use `expire_after: 90000` (25 hours) for deep-sleep compatibility.

## Nextcloud Image Setup

### Image Requirements

- **Format**: PNG (8-bit RGB)
- **Resolution**: 800x480 (or will be scaled to fit)
- **Max size**: 500KB
- **Colors**: Automatically quantized to 7-color palette

### Folder Structure

```
Nextcloud/
├── Photos/EPaper/
│   ├── display.png          # Primary image
│   └── fallback.png         # Backup image
└── Shared/firmware/photonframe/
    └── firmware.bin         # OTA updates
```

## Troubleshooting

### Excessive Battery Drain / Negative Solar Current

If you observe:
- Battery voltage steadily declining with no recovery
- Solar current readings frequently negative (e.g., -50 to -150 µA)
- Short sleep intervals despite low/no sunlight

**Cause:** Prior to v1.0.2, a bug in `readPower()` used `abs(current)` instead of `max(0, current)`. This made negative (reverse) current appear as positive power, triggering shorter sleep intervals even when the solar panel wasn't actually charging the battery.

**Solution:** Update to firmware v1.0.2 or later. The fix ensures negative current = 0 usable power, which correctly triggers longer sleep intervals (8-24 hours) when solar isn't providing charge.

**Understanding negative current:** The INA228 on the high side of the solar panel measures current flowing INTO the energy harvester. Negative readings indicate reverse current flow (e.g., input capacitor discharge through the solar panel's internal resistance during low-light conditions). This is normal behavior but should not be counted as usable solar power.

### INA228 Not Detected

Check serial output for:
```
Powering on INA228...
Initializing I2C on SDA=21, SCL=22
Scanning for INA228 at address 0x40...
Manufacturer ID read: 0x5449   <- Should be 0x5449
```

If ID is `0x0000` or `0xFFFF`:
1. Verify GPIO 13 is connected to INA228 VCC
2. Check SDA (GPIO 21) and SCL (GPIO 22) connections
3. Verify INA228 address matches (A0/A1 pins)

### Display Not Updating

1. Check SPI wiring, especially BUSY pin (GPIO 4)
2. Verify display VCC is 3.3V (not 5V!)
3. Check serial output for PNG decode errors
4. Ensure image is valid PNG, under 500KB

### MQTT Unavailable in Home Assistant

1. Device uses `expire_after` instead of availability topic
2. Sensors stay valid for 25 hours during deep sleep
3. Send `{"action": "force_ha_discovery"}` to republish configs

### WiFi Connection Issues

1. Device creates `PhotonFrame-Setup` AP if WiFi fails
2. Connect to AP, configure via captive portal
3. Check credentials in `secrets.h`

## Configuration Reference

### secrets.h

```cpp
#define SECRET_WIFI_SSID     "YourWiFi"
#define SECRET_WIFI_PASSWORD "YourPassword"

#define SECRET_MQTT_BROKER    "192.168.1.100"
#define SECRET_MQTT_PORT      1883
#define SECRET_MQTT_USERNAME  "mqtt_user"
#define SECRET_MQTT_PASSWORD  "mqtt_pass"
#define SECRET_MQTT_CLIENT_ID "photonframe_001"

#define SECRET_NEXTCLOUD_URL      "https://cloud.example.com/remote.php/dav/files/User/Photos/EPaper/"
#define SECRET_NEXTCLOUD_USER     "username"
#define SECRET_NEXTCLOUD_PASS     "app-password"
#define SECRET_NEXTCLOUD_PHOTO    "display.png"
#define SECRET_NEXTCLOUD_FALLBACK "fallback.png"

#define OTA_HOSTNAME "photonframe"
#define OTA_PASSWORD "photonframe123"
```

### config.h Defaults

| Constant | Value | Description |
|----------|-------|-------------|
| `INA228_I2C_ADDR` | 0x40 | I2C address |
| `INA228_SHUNT_RESISTOR` | 0.015 | 15mΩ shunt |
| `VOLTAGE_CRITICAL` | 3.4V | Emergency threshold |
| `VOLTAGE_LOW` | 3.6V | Low battery threshold |
| `VOLTAGE_MEDIUM` | 3.9V | Medium battery threshold |
| `POWER_ABUNDANT_UW` | 100 | µW for "abundant" state |
| `POWER_LOW_UW` | 50 | µW for "low" state |

## Project Structure

```
PhotonFrame/
├── platformio.ini          # Build configuration
├── secrets.h               # Credentials (gitignored)
├── secrets.example.h       # Template
├── README.md
├── src/
│   ├── main.cpp            # Main firmware
│   ├── config.h            # Pin definitions, constants
│   ├── display.h/cpp       # Spectra 6 e-paper driver
│   ├── power.h/cpp         # INA228 driver, sleep logic
│   └── ota.h               # Nextcloud/GitHub OTA
└── docs/
    └── PINOUT.md           # Detailed wiring reference
```

## Changelog

### v1.0.2
- **Fixed:** Power calculation bug that caused excessive battery drain
  - `readPower()` now uses `max(0, current)` instead of `abs(current)`
  - Negative solar current (reverse flow) correctly results in 0 power
  - Longer sleep intervals now properly trigger when solar isn't charging

### v1.0.1
- Fixed PNG display rendering
- Fixed indoor solar power measurement for low-current scenarios

### v1.0.0
- Initial release

## License

MIT License

## Credits

- [GxEPD2](https://github.com/ZinggJM/GxEPD2) - E-paper display library
- [pngle](https://github.com/kikuchan/pngle) - PNG decoder
- [PubSubClient](https://github.com/knolleary/pubsubclient) - MQTT client
- [WiFiManager](https://github.com/tzapu/WiFiManager) - WiFi configuration
