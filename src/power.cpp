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

void powerOnINA228() {
    Serial.println("Powering on INA228...");
    pinMode(INA228_POWER_PIN, OUTPUT);
    digitalWrite(INA228_POWER_PIN, HIGH);
    delay(100);  // Let power stabilize (INA228 needs time to boot)
    Serial.println("INA228 power on complete");
}

void powerOffINA228() {
    digitalWrite(INA228_POWER_PIN, LOW);
    Serial.println("INA228 powered off");
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
    Serial.printf("Initializing I2C on SDA=%d, SCL=%d\n", I2C_SDA_PIN, I2C_SCL_PIN);

    // Initialize I2C bus
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);  // 400kHz I2C
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
    // Mode=1111, VBUSCT=101 (1052µs), VSHCT=101, VTCT=101, AVG=011 (16 avg)
    writeRegister16(INA228_REG_ADC_CONFIG, 0xFB6B);

    // Calculate and set calibration for shunt resistor
    currentLSB = INA228_MAX_CURRENT / 524288.0f;  // 2^19
    uint16_t shuntCal = (uint16_t)(13107.2e6f * currentLSB * INA228_SHUNT_RESISTOR);
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
    return (raw >> 4) * 195.3125e-6f;  // Convert to volts
}

float readShuntVoltage() {
    int32_t raw = readRegister24(INA228_REG_VSHUNT);
    // Sign extend 24-bit to 32-bit
    if (raw & 0x800000) raw |= 0xFF000000;
    return (float)raw / 16.0f * 312.5e-9f;  // Convert to volts
}

float readCurrent() {
    int32_t raw = readRegister24(INA228_REG_CURRENT);
    // Sign extend 24-bit to 32-bit
    if (raw & 0x800000) raw |= 0xFF000000;
    return (float)raw / 16.0f * currentLSB;  // Convert to amps
}

float readPower() {
    // For very low power (indoor solar), the POWER register can underflow to 0
    // Calculate power from V×I for better resolution at low currents
    float voltage = readBusVoltage();
    float current = readCurrent();
    
    // Use absolute value of current (in case of measurement noise)
    if (current < 0) current = -current;
    
    return voltage * current;  // Power in watts
}

float readINATemperature() {
    int16_t raw = readRegister16(INA228_REG_DIETEMP);
    return (float)raw * 7.8125f / 1000.0f;  // Convert to °C
}

// =============================================================================
// BATTERY READING FUNCTIONS
// =============================================================================

float readBatteryVoltage() {
    // FireBeetle ESP32-E has voltage divider on GPIO34
    // ADC is 12-bit (0-4095), reference is 3.3V
    int adcValue = analogRead(BAT_ADC_PIN);
    return adcValue * BAT_ADC_MULTIPLIER * BAT_ADC_VREF / 4095.0f;
}

int calculateBatteryPercent(float voltage) {
    // LiPo battery: 3.0V = 0%, 4.2V = 100%
    const float minV = 3.0f;
    const float maxV = 4.2f;

    float percent = ((voltage - minV) / (maxV - minV)) * 100.0f;

    if (percent > 100.0f) percent = 100.0f;
    if (percent < 0.0f) percent = 0.0f;

    return (int)percent;
}

// =============================================================================
// POWER STATE CALCULATION (HYBRID ALGORITHM)
// =============================================================================

PowerState calculatePowerState(float batteryVoltage, float solarPowerUW) {
    // Emergency mode - battery critical
    if (batteryVoltage < VOLTAGE_CRITICAL) {
        return POWER_EMERGENCY;
    }

    // Low battery tier
    if (batteryVoltage < VOLTAGE_LOW) {
        return (solarPowerUW > POWER_LOW_UW) ? POWER_CONSERVING : POWER_LOW;
    }

    // Medium battery tier
    if (batteryVoltage < VOLTAGE_MEDIUM) {
        return (solarPowerUW > POWER_ABUNDANT_UW) ? POWER_NEUTRAL : POWER_CONSERVING;
    }

    // Full battery tier
    return (solarPowerUW > POWER_ABUNDANT_UW) ? POWER_ABUNDANT : POWER_NEUTRAL;
}

uint32_t calculateSleepSeconds(float batteryVoltage, float solarPowerUW) {
    PowerState state = calculatePowerState(batteryVoltage, solarPowerUW);

    switch (state) {
        case POWER_ABUNDANT:
            Serial.printf("Power state: ABUNDANT (bat=%.2fV, solar=%.1fuW) -> %d sec\n",
                          batteryVoltage, solarPowerUW, SLEEP_ABUNDANT_SECONDS);
            return SLEEP_ABUNDANT_SECONDS;

        case POWER_NEUTRAL:
            Serial.printf("Power state: NEUTRAL (bat=%.2fV, solar=%.1fuW) -> %d sec\n",
                          batteryVoltage, solarPowerUW, SLEEP_NEUTRAL_SECONDS);
            return SLEEP_NEUTRAL_SECONDS;

        case POWER_CONSERVING:
            Serial.printf("Power state: CONSERVING (bat=%.2fV, solar=%.1fuW) -> %d sec\n",
                          batteryVoltage, solarPowerUW, SLEEP_CONSERVING_SECONDS);
            return SLEEP_CONSERVING_SECONDS;

        case POWER_LOW:
            Serial.printf("Power state: LOW (bat=%.2fV, solar=%.1fuW) -> %d sec\n",
                          batteryVoltage, solarPowerUW, SLEEP_LOW_SECONDS);
            return SLEEP_LOW_SECONDS;

        case POWER_EMERGENCY:
        default:
            Serial.printf("Power state: EMERGENCY (bat=%.2fV) -> %d sec\n",
                          batteryVoltage, SLEEP_EMERGENCY_SECONDS);
            return SLEEP_EMERGENCY_SECONDS;
    }
}

// =============================================================================
// COMBINED READING FUNCTION
// =============================================================================

PowerReadings readAllPowerMetrics() {
    PowerReadings readings = {0};

    // Read INA228 measurements
    if (ina228Initialized) {
        // Wait for ADC conversion with averaging
        delay(200);

        readings.busVoltage = readBusVoltage();
        readings.shuntVoltage = readShuntVoltage();
        readings.current = readCurrent();
        readings.power = readPower();
        readings.temperature = readINATemperature();

        Serial.printf("INA228: V=%.3fV, I=%.6fA, P=%.6fW, T=%.1f°C\n",
                      readings.busVoltage, readings.current,
                      readings.power, readings.temperature);
    }

    // Read battery voltage
    readings.batteryVoltage = readBatteryVoltage();
    readings.batteryPercent = calculateBatteryPercent(readings.batteryVoltage);

    Serial.printf("Battery: %.2fV (%d%%)\n",
                  readings.batteryVoltage, readings.batteryPercent);

    // Calculate power state and sleep duration
    float solarPowerUW = readings.power * 1000000.0f;  // W to µW
    readings.powerState = calculatePowerState(readings.batteryVoltage, solarPowerUW);
    readings.sleepSeconds = calculateSleepSeconds(readings.batteryVoltage, solarPowerUW);

    return readings;
}
