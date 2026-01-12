#ifndef POWER_H
#define POWER_H

#include <Arduino.h>
#include "config.h"

// =============================================================================
// POWER READINGS STRUCTURE
// =============================================================================
struct PowerReadings {
    float busVoltage;       // Voltage in V (solar panel voltage)
    float shuntVoltage;     // Shunt voltage in V
    float current;          // Current in A
    float power;            // Power in W
    float temperature;      // INA228 die temperature in °C
    float batteryVoltage;   // FireBeetle battery voltage in V
    int batteryPercent;     // Battery percentage 0-100
    PowerState powerState;  // Calculated power state
    uint32_t sleepSeconds;  // Calculated sleep duration
};

// =============================================================================
// INA228 FUNCTIONS
// =============================================================================

// Initialize INA228 and I2C bus
// Returns true if INA228 is detected and configured
bool initINA228();

// Power on/off the INA228 via GPIO
void powerOnINA228();
void powerOffINA228();

// Put INA228 into shutdown mode (lower power than power off GPIO)
void sleepINA228();

// Read individual measurements
float readBusVoltage();      // Returns voltage in V
float readShuntVoltage();    // Returns shunt voltage in V
float readCurrent();         // Returns current in A
float readPower();           // Returns power in W
float readINATemperature();  // Returns temperature in °C

// =============================================================================
// BATTERY FUNCTIONS
// =============================================================================

// Read FireBeetle battery voltage via ADC
float readBatteryVoltage();

// Calculate battery percentage from voltage
int calculateBatteryPercent(float voltage);

// Check if battery is critically low and needs immediate shutdown
// Returns true if voltage is below VOLTAGE_SHUTDOWN threshold
bool isBatteryDangerous();

// Check if battery is critically low (below VOLTAGE_CRITICAL)
bool isBatteryCritical();

// =============================================================================
// POWER STATE CALCULATION
// =============================================================================

// Determine power state from battery voltage and solar power
PowerState calculatePowerState(float batteryVoltage, float solarPowerUW);

// Calculate sleep duration based on power state
uint32_t calculateSleepSeconds(float batteryVoltage, float solarPowerUW);

// =============================================================================
// COMBINED READING FUNCTION
// =============================================================================

// Read all power metrics and calculate state
// This is the main function to call from main.cpp
PowerReadings readAllPowerMetrics();

#endif // POWER_H
