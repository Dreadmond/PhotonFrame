#include "power.h"
#include "config.h"
#include <Wire.h>

// =============================================================================
// INA228 REGISTER DEFINITIONS
// =============================================================================
#define INA228_REG_CONFIG 0x00
#define INA228_REG_ADC_CONFIG 0x01
#define INA228_REG_SHUNT_CAL 0x02
#define INA228_REG_VSHUNT 0x04
#define INA228_REG_VBUS 0x05
#define INA228_REG_DIETEMP 0x06
#define INA228_REG_CURRENT 0x07
#define INA228_REG_POWER 0x08
#define INA228_REG_MANUFACTURER_ID 0x3E
#define INA228_REG_DEVICE_ID 0x3F

// =============================================================================
// MODULE STATE
// =============================================================================
static float currentLSB = 0;
static bool ina228Initialized = false;

// =============================================================================
// LOW-LEVEL I2C FUNCTIONS
// =============================================================================

static void writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA228_I2C_ADDR);
  Wire.write(reg);
  Wire.write((value >> 8) & 0xFF);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

static uint16_t readRegister16(uint8_t reg) {
  Wire.beginTransmission(INA228_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(INA228_I2C_ADDR, (uint8_t)2);
  uint16_t value = Wire.read() << 8;
  value |= Wire.read();
  return value;
}

static uint32_t readRegister24(uint8_t reg) {
  Wire.beginTransmission(INA228_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(INA228_I2C_ADDR, (uint8_t)3);
  uint32_t value = (uint32_t)Wire.read() << 16;
  value |= (uint32_t)Wire.read() << 8;
  value |= Wire.read();
  return value;
}

// =============================================================================
// INA228 POWER CONTROL
// =============================================================================

#include <driver/gpio.h>
#include <driver/rtc_io.h>

void powerOnINA228() {
  Serial.println("Powering on INA228...");

  // Release INA228 power pin hold from sleep (GPIO 13 is RTC-capable)
  if (rtc_gpio_is_valid_gpio((gpio_num_t)INA228_POWER_PIN)) {
    rtc_gpio_hold_dis((gpio_num_t)INA228_POWER_PIN);
  }

  // I2C pins (GPIO 21/22) are NOT RTC-capable - no hold/deinit needed
  // Just restore them to normal state for Wire.begin()
  pinMode(I2C_SDA_PIN, INPUT);
  pinMode(I2C_SCL_PIN, INPUT);

  pinMode(INA228_POWER_PIN, OUTPUT);
  digitalWrite(INA228_POWER_PIN, HIGH);
  delay(100); // Let power stabilize
  Serial.println("INA228 power on complete");
}

void powerOffINA228() {
  // Put INA228 into shutdown mode before cutting power
  if (ina228Initialized) {
    sleepINA228();
    delay(10);
  }

  // Cut INA228 VCC first, then isolate I2C pins to prevent backpowering
  // via ESD protection diodes on the INA228's SDA/SCL inputs
  digitalWrite(INA228_POWER_PIN, LOW);

  // I2C pins (non-RTC GPIOs 21/22) - pulldown to prevent floating/backpower
  // With INA228 VCC cut, floating I2C lines could source current through
  // the INA228's internal ESD diodes. Pulldown keeps them at GND.
  pinMode(I2C_SDA_PIN, INPUT_PULLDOWN);
  pinMode(I2C_SCL_PIN, INPUT_PULLDOWN);

  // Hold the power pin LOW during deep sleep (GPIO 13 is RTC-capable)
  if (rtc_gpio_is_valid_gpio((gpio_num_t)INA228_POWER_PIN)) {
    rtc_gpio_hold_en((gpio_num_t)INA228_POWER_PIN);
  }

  Serial.println("INA228 powered off, I2C pins pulled low");
}

void sleepINA228() {
  // Put INA228 into shutdown mode (MODE=0x0 in ADC_CONFIG)
  writeRegister16(INA228_REG_ADC_CONFIG, 0x0000);
  Serial.println("INA228 in shutdown mode");
}

// =============================================================================
// INA228 INITIALIZATION
// =============================================================================

bool initINA228() {
  Serial.printf("Initializing I2C on SDA=%d, SCL=%d\n", I2C_SDA_PIN,
                I2C_SCL_PIN);

  // Initialize I2C bus
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); // 400kHz I2C
  delay(10);

  Serial.printf("Scanning for INA228 at address 0x%02X...\n", INA228_I2C_ADDR);

  // Check manufacturer ID
  uint16_t mfgId = readRegister16(INA228_REG_MANUFACTURER_ID);
  Serial.printf("Manufacturer ID read: 0x%04X\n", mfgId);

  if (mfgId != 0x5449) {
    Serial.printf("INA228 not found! (ID: 0x%04X, expected 0x5449)\n", mfgId);
    return false;
  }

  Serial.printf("INA228 detected (Manufacturer ID: 0x%04X)\n", mfgId);

  // Reset the device
  writeRegister16(INA228_REG_CONFIG, 0x8000);
  delay(10);

  // Configure ADC: continuous mode, all measurements
  // Mode=1111, VBUSCT=101 (1052µs), VSHCT=101, VTCT=101, AVG=101 (128 avg)
  writeRegister16(INA228_REG_ADC_CONFIG, 0xFBAD);

  // Calculate and set calibration for shunt resistor
  currentLSB = INA228_MAX_CURRENT / 524288.0f; // 2^19
  uint16_t shuntCal =
      (uint16_t)(13107.2e6f * currentLSB * INA228_SHUNT_RESISTOR);
  writeRegister16(INA228_REG_SHUNT_CAL, shuntCal);

  Serial.printf("INA228 configured: shuntCal=0x%04X, currentLSB=%.9f\n",
                shuntCal, currentLSB);

  ina228Initialized = true;
  return true;
}

// =============================================================================
// INA228 READING FUNCTIONS
// =============================================================================

float readBusVoltage() {
  uint32_t raw = readRegister24(INA228_REG_VBUS);
  return (raw >> 4) * 195.3125e-6f; // Convert to volts
}

float readShuntVoltage() {
  int32_t raw = readRegister24(INA228_REG_VSHUNT);
  // Sign extend 24-bit to 32-bit
  if (raw & 0x800000)
    raw |= 0xFF000000;
  return (float)raw / 16.0f * 312.5e-9f; // Convert to volts
}

float readCurrent() {
  int32_t raw = readRegister24(INA228_REG_CURRENT);
  // Sign extend 24-bit to 32-bit
  if (raw & 0x800000)
    raw |= 0xFF000000;
  float current = (float)raw / 16.0f * currentLSB; // Convert to amps

  // Clamp negative current to zero (solar panels only source current)
  if (current < 0.0f) {
    current = 0.0f;
  }
  return current;
}

float readPower() {
  // Calculate power from V×I for best resolution at low indoor currents
  float voltage = readBusVoltage();
  float current = readCurrent(); // already clamped to noise floor

  return voltage * current; // Power in watts
}

float readINATemperature() {
  int16_t raw = readRegister16(INA228_REG_DIETEMP);
  return (float)raw * 7.8125f / 1000.0f; // Convert to °C
}

// =============================================================================
// BATTERY READING FUNCTIONS
// =============================================================================

float readBatteryVoltage() {
  // FireBeetle ESP32-E has voltage divider on GPIO34
  // ADC is 12-bit (0-4095), reference is 3.3V
  // Average 8 readings to reduce ESP32 ADC noise (±100mV single-read)
  float sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delayMicroseconds(500);
  }
  float adcValue = sum / 8.0f;
  return adcValue * BAT_ADC_MULTIPLIER * BAT_ADC_VREF / 4095.0f;
}

int calculateBatteryPercent(float voltage) {
  // LiPo discharge curve lookup table (non-linear)
  // Based on typical single-cell LiPo at low discharge rates
  static const float curve[][2] = {
      {4.20f, 100.0f}, {4.15f, 95.0f}, {4.10f, 90.0f}, {4.05f, 85.0f},
      {4.00f, 80.0f},  {3.95f, 75.0f}, {3.90f, 70.0f}, {3.85f, 60.0f},
      {3.80f, 50.0f},  {3.75f, 40.0f}, {3.70f, 30.0f}, {3.65f, 20.0f},
      {3.60f, 15.0f},  {3.50f, 10.0f}, {3.40f, 5.0f},  {3.20f, 1.0f},
      {3.00f, 0.0f},
  };
  static const int curveLen = sizeof(curve) / sizeof(curve[0]);

  if (voltage >= curve[0][0])
    return 100;
  if (voltage <= curve[curveLen - 1][0])
    return 0;

  // Linear interpolation between curve points
  for (int i = 0; i < curveLen - 1; i++) {
    if (voltage >= curve[i + 1][0]) {
      float v1 = curve[i][0], p1 = curve[i][1];
      float v2 = curve[i + 1][0], p2 = curve[i + 1][1];
      return (int)(p2 + (p1 - p2) * (voltage - v2) / (v1 - v2));
    }
  }
  return 0;
}

bool isBatteryDangerous() {
  // Take multiple readings to avoid false triggers from ADC noise
  float sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += readBatteryVoltage();
    delay(10);
  }
  float avgVoltage = sum / 5.0f;

  if (avgVoltage < VOLTAGE_SHUTDOWN) {
    Serial.printf(
        "DANGER: Battery at %.2fV (threshold: %.2fV) - SHUTDOWN REQUIRED\n",
        avgVoltage, VOLTAGE_SHUTDOWN);
    return true;
  }
  return false;
}

bool isBatteryCritical() {
  float voltage = readBatteryVoltage();
  if (voltage < VOLTAGE_CRITICAL) {
    Serial.printf("WARNING: Battery at %.2fV (critical threshold: %.2fV)\n",
                  voltage, VOLTAGE_CRITICAL);
    return true;
  }
  return false;
}

// =============================================================================
// ADAPTIVE SLEEP ALGORITHM v2
// =============================================================================
// Tracks voltage history over multiple boots for reliable trend detection.
// Uses absolute voltage levels to set minimum intervals.
// Any drain triggers longer sleep - proportional to drain rate.

#define VOLTAGE_HISTORY_SIZE 4

// RTC memory survives deep sleep - stores state between boots
RTC_DATA_ATTR float voltageHistory[VOLTAGE_HISTORY_SIZE] = {0};
RTC_DATA_ATTR uint8_t historyIndex = 0;
RTC_DATA_ATTR uint8_t historyCount = 0;
RTC_DATA_ATTR uint32_t currentSleepInterval = SLEEP_DEFAULT_SECONDS;
RTC_DATA_ATTR bool adaptiveInitialized = false;
RTC_DATA_ATTR PowerState lastPowerState = POWER_STABLE;

void initAdaptiveSleep() {
  if (!adaptiveInitialized) {
    for (int i = 0; i < VOLTAGE_HISTORY_SIZE; i++) {
      voltageHistory[i] = 0;
    }
    historyIndex = 0;
    historyCount = 0;
    currentSleepInterval = SLEEP_DEFAULT_SECONDS;
    adaptiveInitialized = true;
    Serial.println("Adaptive sleep v2: Initialized");
  }
}

PowerState getCurrentPowerState() { return lastPowerState; }

// Get minimum sleep interval based on absolute voltage level
static uint32_t getMinimumSleepForVoltage(float voltage) {
  if (voltage < VOLTAGE_CRITICAL) {
    return SLEEP_EMERGENCY_SECONDS; // 24 hours
  } else if (voltage < VOLTAGE_LOW) {
    return SLEEP_LOW_SECONDS; // 8 hours
  } else if (voltage < VOLTAGE_MEDIUM) {
    return SLEEP_MEDIUM_SECONDS; // 4 hours
  } else {
    return SLEEP_MIN_SECONDS; // 1 hour (plenty of power)
  }
}

// Calculate average voltage change per cycle from history
static float calculateTrend() {
  if (historyCount < 2) {
    return 0.0f; // Not enough data
  }

  // Get oldest and newest readings
  // historyIndex points to where we'll write NEXT, so current is (historyIndex
  // - 1)
  int newestIdx =
      (historyIndex - 1 + VOLTAGE_HISTORY_SIZE) % VOLTAGE_HISTORY_SIZE;
  int oldestIdx = (historyIndex - historyCount + VOLTAGE_HISTORY_SIZE) %
                  VOLTAGE_HISTORY_SIZE;

  float newest = voltageHistory[newestIdx];
  float oldest = voltageHistory[oldestIdx];

  // Average change per cycle
  float totalDelta = newest - oldest;
  float avgDeltaPerCycle = totalDelta / (historyCount - 1);

  return avgDeltaPerCycle;
}

uint32_t calculateAdaptiveSleep(float currentVoltage) {
  Serial.println("\n--- Adaptive Sleep v2 ---");
  Serial.printf("Current voltage: %.3fV\n", currentVoltage);

  // Store voltage in history
  voltageHistory[historyIndex] = currentVoltage;
  historyIndex = (historyIndex + 1) % VOLTAGE_HISTORY_SIZE;
  if (historyCount < VOLTAGE_HISTORY_SIZE) {
    historyCount++;
  }

  // Debug: print history
  Serial.print("History [");
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + VOLTAGE_HISTORY_SIZE) %
              VOLTAGE_HISTORY_SIZE;
    Serial.printf("%.3f", voltageHistory[idx]);
    if (i < historyCount - 1)
      Serial.print(", ");
  }
  Serial.println("]");

  // STEP 1: Get minimum interval based on absolute voltage
  uint32_t minInterval = getMinimumSleepForVoltage(currentVoltage);
  Serial.printf("Voltage-based minimum: %d sec (%.1f hours)\n", minInterval,
                minInterval / 3600.0f);

  // Emergency: below critical, use emergency interval immediately
  if (currentVoltage < VOLTAGE_CRITICAL) {
    lastPowerState = POWER_EMERGENCY;
    currentSleepInterval = SLEEP_EMERGENCY_SECONDS;
    Serial.printf("EMERGENCY: voltage %.2fV < %.2fV -> %d sec\n",
                  currentVoltage, VOLTAGE_CRITICAL, currentSleepInterval);
    return currentSleepInterval;
  }

  // STEP 2: Calculate trend from history
  float trend = calculateTrend();
  Serial.printf("Voltage trend: %+.4fV per cycle\n", trend);

  // STEP 3: Determine power state and interval adjustment
  uint32_t newInterval = currentSleepInterval;

  if (trend > VOLTAGE_CHARGING_THRESHOLD) {
    // Charging - battery recovering
    lastPowerState = POWER_CHARGING;
    // Decrease interval by 20%, but respect minimum
    newInterval = (uint32_t)(currentSleepInterval * 0.8f);
    Serial.printf("CHARGING: trend %+.4fV -> interval x0.8\n", trend);

  } else if (trend < -0.001f) {
    // ANY measurable drain - increase interval proportionally
    // More drain = bigger increase
    float drainRate = -trend; // Make positive for easier math

    if (drainRate > 0.02f) {
      // Fast drain (>20mV/cycle): double interval
      lastPowerState = POWER_CRITICAL;
      newInterval = currentSleepInterval * 2;
      Serial.printf("DRAINING FAST: %.1fmV/cycle -> interval x2\n",
                    drainRate * 1000);

    } else if (drainRate > 0.01f) {
      // Medium drain (10-20mV/cycle): +50%
      lastPowerState = POWER_DRAINING;
      newInterval = (uint32_t)(currentSleepInterval * 1.5f);
      Serial.printf("DRAINING MEDIUM: %.1fmV/cycle -> interval x1.5\n",
                    drainRate * 1000);

    } else {
      // Slow drain (<10mV/cycle): +25%
      lastPowerState = POWER_DRAINING;
      newInterval = (uint32_t)(currentSleepInterval * 1.25f);
      Serial.printf("DRAINING SLOW: %.1fmV/cycle -> interval x1.25\n",
                    drainRate * 1000);
    }

  } else {
    // Stable (within noise floor)
    lastPowerState = POWER_STABLE;
    Serial.println("STABLE: trend within noise floor");
  }

  // STEP 4: Apply bounds
  // Never go below voltage-based minimum
  if (newInterval < minInterval) {
    Serial.printf("Clamping to voltage minimum: %d -> %d sec\n", newInterval,
                  minInterval);
    newInterval = minInterval;
  }

  // Never exceed maximum
  if (newInterval > SLEEP_MAX_SECONDS) {
    newInterval = SLEEP_MAX_SECONDS;
  }

  currentSleepInterval = newInterval;

  Serial.printf("RESULT: %d sec (%.1f hours), state: %s\n",
                currentSleepInterval, currentSleepInterval / 3600.0f,
                getPowerStateName(lastPowerState));
  Serial.println("-------------------------\n");

  return currentSleepInterval;
}

// =============================================================================
// COMBINED READING FUNCTION
// =============================================================================

PowerReadings readAllPowerMetrics() {
  PowerReadings readings = {0};

  // Initialize adaptive sleep on first call
  initAdaptiveSleep();

  // Read INA228 measurements (for telemetry, not for sleep decisions)
  if (ina228Initialized) {
    // Wait for ADC conversion with averaging
    delay(200);

    readings.busVoltage = readBusVoltage();
    readings.shuntVoltage = readShuntVoltage();
    readings.current = readCurrent();
    readings.power = readPower();
    readings.temperature = readINATemperature();

    Serial.printf("INA228: V=%.3fV, I=%.6fA, P=%.6fW, T=%.1f°C\n",
                  readings.busVoltage, readings.current, readings.power,
                  readings.temperature);
  }

  // Read battery voltage
  readings.batteryVoltage = readBatteryVoltage();
  readings.batteryPercent = calculateBatteryPercent(readings.batteryVoltage);

  Serial.printf("Battery: %.2fV (%d%%)\n", readings.batteryVoltage,
                readings.batteryPercent);

  // Use adaptive algorithm - sleep based on voltage trend, not solar
  readings.sleepSeconds = calculateAdaptiveSleep(readings.batteryVoltage);
  readings.powerState = getCurrentPowerState();

  return readings;
}
