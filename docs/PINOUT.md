# PhotonFrame Pinout Reference

## FireBeetle ESP32-E Pin Assignments

### I2C Bus (INA228 Power Sensor)

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | 21 | I2C Data |
| SCL | 22 | I2C Clock |
| VCC | 13 | Switched power (HIGH=on, LOW=off) |

### SPI Bus (Spectra 6 E-Paper Display)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCK | 18 | SPI Clock |
| MOSI | 23 | SPI Data Out (DIN on display) |
| MISO | 19 | SPI Data In (not used by display) |
| CS | 5 | Chip Select (active low) |
| DC | 17 | Data/Command select |
| RST | 16 | Reset (active low) |
| BUSY | 4 | Busy status from display |

### Power Monitoring

| Signal | GPIO | Notes |
|--------|------|-------|
| Battery ADC | 34 | Built-in 2:1 voltage divider |

### Status

| Signal | GPIO | Notes |
|--------|------|-------|
| LED | 2 | Built-in blue LED |

## Wiring Diagram

```
                     FireBeetle ESP32-E
                    ┌───────────────────┐
                    │                   │
    INA228          │                   │     7.3" Spectra 6 Display
   ┌───────┐        │                   │     ┌───────────────────┐
   │ VCC ──┼────────┤ GPIO 13           │     │                   │
   │ GND ──┼────────┤ GND               │     │ VCC ──────────────┤── 3.3V
   │ SDA ──┼────────┤ GPIO 21      GPIO 18 ├───┤ CLK              │
   │ SCL ──┼────────┤ GPIO 22      GPIO 23 ├───┤ DIN (MOSI)       │
   │       │        │               GPIO 5 ├───┤ CS               │
   │ VIN+ ─┼── +    │              GPIO 17 ├───┤ DC               │
   │ VIN- ─┼── -    │              GPIO 16 ├───┤ RST              │
   └───────┘        │               GPIO 4 ├───┤ BUSY             │
       │            │                  GND ├───┤ GND              │
   Solar Panel      │                   │     └───────────────────┘
                    └───────────────────┘
```

## INA228 Wiring Detail

The INA228 measures current through its shunt resistor (15mΩ on Adafruit boards):

```
Solar Panel (+) ──────► VIN+ ┌─────────┐
                             │  INA228 │
                      VIN- ──┤         ├── VCC ──► GPIO 13
                        │    │         ├── GND ──► GND
                        │    │         ├── SDA ──► GPIO 21
                        │    │         ├── SCL ──► GPIO 22
                        │    └─────────┘
                        │
                        └──► To battery charge circuit
```

## I2C Address Configuration

The INA228 I2C address is configurable via A0/A1 pins:

| A1 | A0 | Address |
|----|----|---------|
| GND | GND | 0x40 (default) |
| GND | VCC | 0x41 |
| VCC | GND | 0x44 |
| VCC | VCC | 0x45 |

The firmware uses address 0x40 by default. Change `INA228_I2C_ADDR` in `config.h` if needed.

## Display Power Notes

The Spectra 6 7.3" display:
- Operates at 3.3V (NOT 5V!)
- Draws ~80mA during refresh (~20 seconds)
- ~0µA in hibernation mode
- Total energy per refresh: ~0.4mAh

## Sleep Current Optimization

During deep sleep, the firmware:
1. Powers off INA228 via GPIO 13 (saves ~5mA)
2. Puts display in hibernate mode
3. Sets LED pin to INPUT (floating)
4. Disables WiFi radio

Expected deep sleep current: ~10µA
