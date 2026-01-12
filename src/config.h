#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// FIRMWARE VERSION
// =============================================================================
#define FIRMWARE_VERSION "1.0.3"
#define FIRMWARE_BUILD_DATE __DATE__ " " __TIME__

// =============================================================================
// DEBUG OUTPUT
// =============================================================================
#ifdef NDEBUG
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
#else
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#endif

// =============================================================================
// DEVICE IDENTIFICATION
// =============================================================================
#define DEVICE_NAME "PhotonFrame"
#define DEVICE_ID "photonframe_001"
#define DEVICE_MODEL "PhotonFrame v1"
#define DEVICE_MANUFACTURER "DIY"

// =============================================================================
// GPIO PIN DEFINITIONS - FireBeetle ESP32-E
// =============================================================================

// I2C Pins (INA228)
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// INA228 Power Control (allows complete power-off during sleep)
#define INA228_POWER_PIN 13

// SPI Pins (Spectra 6 Display)
#define SPI_SCK_PIN 18
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19  // Not used by display, but defined for completeness

// E-Paper Display Control Pins
#define EPD_CS_PIN 3
#define EPD_DC_PIN 17
#define EPD_RST_PIN 16
#define EPD_BUSY_PIN 4

// Battery Voltage ADC (FireBeetle ESP32-E built-in voltage divider)
#define BAT_ADC_PIN 34

// Status LED (FireBeetle ESP32-E built-in)
#define LED_PIN 2

// =============================================================================
// INA228 CONFIGURATION
// =============================================================================
#define INA228_I2C_ADDR 0x40     // Default address (A0=GND, A1=GND)
#define INA228_SHUNT_RESISTOR 0.015f  // 15mΩ shunt (Adafruit INA228)
#define INA228_MAX_CURRENT 0.1f  // Maximum expected current in Amps (100mA)

// =============================================================================
// BATTERY VOLTAGE THRESHOLDS
// =============================================================================
// FireBeetle ESP32-E has 2:1 voltage divider on GPIO34
#define BAT_ADC_MULTIPLIER 2.0f
#define BAT_ADC_VREF 3.3f

// Voltage thresholds for power state decisions
#define VOLTAGE_SHUTDOWN 3.2f    // Below: HARD SHUTDOWN - no wake until manual reset/charge
#define VOLTAGE_CRITICAL 3.4f    // Below: emergency mode (24h sleep, skip display)
#define VOLTAGE_LOW 3.6f         // Below: conserving mode
#define VOLTAGE_MEDIUM 3.9f      // Below: neutral mode
#define VOLTAGE_FULL 4.1f        // Above: abundant mode

// =============================================================================
// SOLAR POWER THRESHOLDS (µW)
// =============================================================================
#define POWER_ABUNDANT_UW 100.0f  // Good indoor light
#define POWER_LOW_UW 50.0f        // Minimal light

// =============================================================================
// SLEEP INTERVALS (seconds)
// =============================================================================
// Based on power state matrix
#define SLEEP_ABUNDANT_SECONDS (30 * 60)       // 30 minutes
#define SLEEP_NEUTRAL_SECONDS (2 * 60 * 60)    // 2 hours
#define SLEEP_CONSERVING_SECONDS (4 * 60 * 60) // 4 hours
#define SLEEP_LOW_SECONDS (8 * 60 * 60)        // 8 hours
#define SLEEP_EMERGENCY_SECONDS (24 * 60 * 60) // 24 hours

// WiFi retry sleep
#define WIFI_RETRY_SLEEP_SECONDS (30 * 60)     // 30 minutes

// =============================================================================
// DISPLAY CONFIGURATION
// =============================================================================
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480
#define DISPLAY_REFRESH_MS (20 * 1000UL)  // 20 seconds for Spectra 6 refresh

// =============================================================================
// MQTT CONFIGURATION
// =============================================================================
#define MQTT_TOPIC_PREFIX "photonframe"
#define MQTT_TOPIC_STATE "photonframe/state"
#define MQTT_TOPIC_AVAILABILITY "photonframe/availability"
#define MQTT_TOPIC_COMMAND "photonframe/command"
#define MQTT_TOPIC_STATUS "photonframe/status"

#define MQTT_BUFFER_SIZE 2048
#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 15

// =============================================================================
// OTA CONFIGURATION
// =============================================================================
// Nextcloud firmware path (relative to WebDAV root)
#define OTA_NEXTCLOUD_PATH "/Shared/firmware/photonframe/firmware.bin"

// GitHub repository for fallback OTA
#define OTA_GITHUB_USER "PhotonFrame"  // Override in secrets.h
#define OTA_GITHUB_REPO "PhotonFrame"
#define OTA_GITHUB_TAG "v1.0.0"
#define OTA_FIRMWARE_FILENAME "firmware.bin"

// =============================================================================
// NEXTCLOUD IMAGE CONFIGURATION
// =============================================================================
#define NEXTCLOUD_MAX_IMAGE_SIZE 500000  // 500KB max
#define NEXTCLOUD_TIMEOUT_MS 60000       // 60 second timeout
#define NEXTCLOUD_CHUNK_SIZE 4096        // 4KB download chunks

// =============================================================================
// TIMING CONFIGURATION
// =============================================================================
#define WIFI_CONNECT_TIMEOUT_MS 15000    // 15 seconds
#define WIFI_CONNECT_ATTEMPTS 30         // Number of 500ms attempts

// Watchdog timeout - force sleep after this time
#define WATCHDOG_TIMEOUT_MS 120000       // 2 minutes

// Home Assistant discovery republish interval (in boot cycles)
#define HA_DISCOVERY_INTERVAL 20

// =============================================================================
// POWER STATE ENUMERATION
// =============================================================================
enum PowerState {
    POWER_ABUNDANT,    // Battery full, good solar
    POWER_NEUTRAL,     // Battery good, some solar
    POWER_CONSERVING,  // Battery medium/low, minimal solar
    POWER_LOW,         // Battery low, no solar
    POWER_EMERGENCY    // Battery critical
};

// Power state names for MQTT
inline const char* getPowerStateName(PowerState state) {
    switch (state) {
        case POWER_ABUNDANT: return "abundant";
        case POWER_NEUTRAL: return "neutral";
        case POWER_CONSERVING: return "conserving";
        case POWER_LOW: return "low";
        case POWER_EMERGENCY: return "emergency";
        default: return "unknown";
    }
}

#endif // CONFIG_H
