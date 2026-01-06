/*
 * PhotonFrame - Indoor Solar E-Paper Display
 *
 * Features:
 * - INA228 solar power monitoring with hybrid refresh scheduling
 * - 7.3" Spectra 6 (7-color) e-paper display
 * - Nextcloud WebDAV image fetching
 * - MQTT integration with Home Assistant autodiscovery
 * - Dual OTA updates (Nextcloud priority, GitHub fallback)
 * - Arduino OTA for development
 */

#include "../secrets.h"
#include "config.h"
#include "display.h"
#include "ota.h"
#include "power.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sleep.h>
#include <esp_system.h>

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
HTTPClient http;
WiFiManager wifiManager;
NextcloudOTA nextcloudOTA;
GitHubOTA githubOTA;

// =============================================================================
// STATE VARIABLES
// =============================================================================
bool wifiConnected = false;
bool mqttConnected = false;
bool otaModeActive = false;
unsigned long bootTime = 0;
PowerReadings powerData = {0};

// Error tracking
uint32_t errorCount = 0;
String lastError = "";

// Arduino OTA window duration (seconds)
#define ARDUINO_OTA_WINDOW_SECONDS 10

// RTC memory (survives deep sleep)
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t totalErrorCount = 0;
RTC_DATA_ATTR uint32_t successfulUpdates = 0;
RTC_DATA_ATTR bool haDiscoveryPublished = false;

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

void logError(const char* error) {
    errorCount++;
    totalErrorCount++;
    lastError = String(error);
    Serial.printf("ERROR [%d]: %s\n", errorCount, error);
}

const char* getResetReason() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON: return "Power on";
        case ESP_RST_SW: return "Software reset";
        case ESP_RST_PANIC: return "Exception/Panic";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT: return "Brownout";
        default: return "Unknown";
    }
}

// =============================================================================
// ARDUINO OTA FUNCTIONS
// =============================================================================

void setupArduinoOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("ArduinoOTA: Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\nArduinoOTA: Update complete!");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("ArduinoOTA: %u%%\r", (progress / (total / 100)));
        // Blink LED during update
        digitalWrite(LED_PIN, (progress / 1000) % 2);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("ArduinoOTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();
    Serial.printf("ArduinoOTA ready: %s.local (password protected)\n", OTA_HOSTNAME);
}

void runArduinoOTAWindow(int seconds) {
    Serial.printf("ArduinoOTA: Listening for %d seconds...\n", seconds);
    unsigned long start = millis();

    while (millis() - start < (seconds * 1000UL)) {
        ArduinoOTA.handle();
        delay(10);
    }

    Serial.println("ArduinoOTA: Window closed");
}

// =============================================================================
// WIFI FUNCTIONS
// =============================================================================

bool connectWiFi() {
    Serial.println("Connecting to WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_ATTEMPTS) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    // Fallback to WiFiManager
    Serial.println("WiFi failed, starting captive portal...");
    wifiManager.setConfigPortalTimeout(180);

    if (!wifiManager.autoConnect("PhotonFrame-Setup")) {
        Serial.println("WiFiManager failed");
        wifiConnected = false;
        return false;
    }

    wifiConnected = true;
    Serial.printf("WiFi connected via portal: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// =============================================================================
// MQTT FUNCTIONS
// =============================================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    Serial.printf("MQTT: %s -> %s\n", topic, message.c_str());

    if (String(topic) == MQTT_TOPIC_COMMAND) {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, message) == DeserializationError::Ok) {
            String action = doc["action"] | "";

            if (action == "update_display") {
                Serial.println("Force display update requested");
                // Will update on next boot
            } else if (action == "reboot") {
                Serial.println("Reboot requested");
                Serial.flush();
                delay(1000);
                esp_restart();
            } else if (action == "check_ota") {
                Serial.println("OTA check requested");
                if (!nextcloudOTA.checkAndUpdate()) {
                    githubOTA.checkAndUpdate();
                }
            } else if (action == "clear_errors") {
                errorCount = 0;
                totalErrorCount = 0;
                lastError = "";
                Serial.println("Errors cleared");
            } else if (action == "force_ha_discovery") {
                haDiscoveryPublished = false;
                Serial.println("HA discovery will republish");
            } else if (action == "ota_mode") {
                Serial.println("Entering OTA mode - staying awake for updates");
                otaModeActive = true;
                mqttClient.publish(MQTT_TOPIC_STATUS, "ota_mode_active", true);
            }
        }
    }
}

bool connectMQTT() {
    if (!wifiConnected) return false;

    Serial.printf("Connecting to MQTT: %s:%d\n", SECRET_MQTT_BROKER, SECRET_MQTT_PORT);

    mqttClient.setServer(SECRET_MQTT_BROKER, SECRET_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);

    if (mqttClient.connect(SECRET_MQTT_CLIENT_ID, SECRET_MQTT_USERNAME, SECRET_MQTT_PASSWORD,
                           MQTT_TOPIC_AVAILABILITY, 0, true, "offline")) {
        Serial.println("MQTT connected");
        mqttConnected = true;
        mqttClient.subscribe(MQTT_TOPIC_COMMAND);
        return true;
    }

    Serial.printf("MQTT failed: %d\n", mqttClient.state());
    mqttConnected = false;
    return false;
}

void publishHADiscovery() {
    if (!mqttConnected) return;
    if (haDiscoveryPublished && bootCount > 1) {
        Serial.println("HA discovery already published");
        return;
    }

    Serial.println("Publishing HA autodiscovery...");

    // Device info for all sensors
    auto addDevice = [](JsonDocument& doc) {
        JsonObject device = doc.createNestedObject("device");
        device["identifiers"][0] = DEVICE_ID;
        device["name"] = DEVICE_NAME;
        device["manufacturer"] = DEVICE_MANUFACTURER;
        device["model"] = DEVICE_MODEL;
        device["sw_version"] = FIRMWARE_VERSION;
    };

    // Sensor definitions
    struct SensorDef {
        const char* id;
        const char* name;
        const char* valueTemplate;
        const char* unit;
        const char* deviceClass;
        const char* stateClass;
        bool isBinary;
    };

    SensorDef sensors[] = {
        {"battery", "Battery", "{{ value_json.battery_percentage }}", "%", "battery", "measurement", false},
        {"voltage", "Battery Voltage", "{{ value_json.battery_voltage }}", "V", "voltage", "measurement", false},
        {"solar_voltage", "Solar Voltage", "{{ value_json.solar_voltage_mV }}", "mV", "voltage", "measurement", false},
        {"solar_current", "Solar Current", "{{ value_json.solar_current_uA }}", "uA", "current", "measurement", false},
        {"solar_power", "Solar Power", "{{ value_json.solar_power_uW }}", "uW", "power", "measurement", false},
        {"ina_temp", "INA Temperature", "{{ value_json.ina_temperature }}", "°C", "temperature", "measurement", false},
        {"power_state", "Power State", "{{ value_json.power_state }}", nullptr, nullptr, nullptr, false},
        {"next_refresh", "Next Refresh", "{{ value_json.next_refresh_seconds }}", "s", "duration", "measurement", false},
        {"wifi_rssi", "WiFi Signal", "{{ value_json.wifi_rssi }}", "dBm", "signal_strength", "measurement", false},
        {"boot_count", "Boot Count", "{{ value_json.boot_count }}", nullptr, nullptr, "total_increasing", false},
        {"version", "Firmware", "{{ value_json.firmware_version }}", nullptr, nullptr, nullptr, false},
    };

    for (auto& sensor : sensors) {
        DynamicJsonDocument doc(1024);
        doc["unique_id"] = String(DEVICE_ID) + "_" + sensor.id;
        doc["name"] = String(DEVICE_NAME) + " " + sensor.name;
        doc["state_topic"] = MQTT_TOPIC_STATE;
        doc["value_template"] = sensor.valueTemplate;
        doc["availability_topic"] = MQTT_TOPIC_AVAILABILITY;

        if (sensor.unit) doc["unit_of_measurement"] = sensor.unit;
        if (sensor.deviceClass) doc["device_class"] = sensor.deviceClass;
        if (sensor.stateClass) doc["state_class"] = sensor.stateClass;

        addDevice(doc);

        String configTopic = "homeassistant/sensor/" + String(DEVICE_ID) + "_" + sensor.id + "/config";
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(configTopic.c_str(), payload.c_str(), true);
    }

    mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
    haDiscoveryPublished = true;

    // Flush MQTT
    for (int i = 0; i < 10; i++) {
        mqttClient.loop();
        delay(50);
    }

    Serial.println("HA discovery published");
}

void publishState() {
    if (!mqttConnected) {
        if (!connectMQTT()) return;
    }

    DynamicJsonDocument doc(1024);

    doc["battery_percentage"] = powerData.batteryPercent;
    doc["battery_voltage"] = round(powerData.batteryVoltage * 100) / 100.0;
    doc["solar_voltage_mV"] = round(powerData.busVoltage * 1000 * 10) / 10.0;
    doc["solar_current_uA"] = round(powerData.current * 1000000 * 10) / 10.0;
    doc["solar_power_uW"] = round(powerData.power * 1000000 * 10) / 10.0;
    doc["ina_temperature"] = round(powerData.temperature * 10) / 10.0;
    doc["power_state"] = getPowerStateName(powerData.powerState);
    doc["next_refresh_seconds"] = powerData.sleepSeconds;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["wifi_connected"] = wifiConnected;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["boot_count"] = bootCount;
    doc["boot_reason"] = getResetReason();
    doc["error_count"] = totalErrorCount;
    doc["last_error"] = lastError.length() > 0 ? lastError : "none";
    doc["successful_updates"] = successfulUpdates;
    doc["ip_address"] = WiFi.localIP().toString();

    String payload;
    serializeJson(doc, payload);

    Serial.printf("Publishing state: %s\n", payload.c_str());
    mqttClient.publish(MQTT_TOPIC_STATE, payload.c_str(), true);
    mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
    mqttClient.loop();
}

// =============================================================================
// NEXTCLOUD IMAGE FETCH
// =============================================================================

bool fetchImageFromNextcloud(uint8_t** imageData, size_t* imageSize) {
    if (!wifiConnected) {
        logError("WiFi not connected");
        return false;
    }

    String url = String(SECRET_NEXTCLOUD_URL) + SECRET_NEXTCLOUD_PHOTO;
    Serial.printf("Fetching image: %s\n", url.c_str());

    http.end();
    delay(100);

    http.begin(url);
    http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
    http.setTimeout(NEXTCLOUD_TIMEOUT_MS);
    http.setReuse(false);

    int httpCode = http.GET();
    Serial.printf("HTTP response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
        *imageSize = http.getSize();
        Serial.printf("Image size: %d bytes\n", *imageSize);

        if (*imageSize > 0 && *imageSize < NEXTCLOUD_MAX_IMAGE_SIZE) {
            *imageData = (uint8_t*)malloc(*imageSize);
            if (*imageData != NULL) {
                WiFiClient* stream = http.getStreamPtr();
                if (stream == NULL) {
                    logError("HTTP stream null");
                    free(*imageData);
                    *imageData = NULL;
                    http.end();
                    return false;
                }

                size_t totalRead = 0;
                unsigned long lastActivity = millis();

                while (totalRead < *imageSize) {
                    if (millis() - lastActivity > 30000) {
                        Serial.println("Stream timeout");
                        break;
                    }

                    if (stream->available() == 0) {
                        delay(10);
                        continue;
                    }

                    size_t toRead = min((size_t)NEXTCLOUD_CHUNK_SIZE, *imageSize - totalRead);
                    size_t bytesRead = stream->readBytes(*imageData + totalRead, toRead);

                    if (bytesRead > 0) {
                        totalRead += bytesRead;
                        lastActivity = millis();

                        if (totalRead % 10240 == 0 || totalRead == *imageSize) {
                            Serial.printf("Downloaded: %d/%d (%.0f%%)\n",
                                          totalRead, *imageSize, (totalRead * 100.0) / *imageSize);
                        }
                    }
                    delay(5);
                }

                http.end();

                if (totalRead == *imageSize) {
                    Serial.println("Image download complete");
                    return true;
                }

                logError("Incomplete download");
                free(*imageData);
                *imageData = NULL;
                return false;
            }
            logError("Memory allocation failed");
        } else {
            logError("Invalid image size");
        }
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "HTTP error: %d", httpCode);
        logError(buf);
    }

    http.end();
    return false;
}

void updateDisplay() {
    Serial.println("Updating display from Nextcloud...");

    uint8_t* imageData = NULL;
    size_t imageSize = 0;

    if (fetchImageFromNextcloud(&imageData, &imageSize)) {
        displayImageOnEPaper(imageData, imageSize);
        successfulUpdates++;
        Serial.println("Display updated successfully");

        if (imageData != NULL) {
            free(imageData);
        }
    } else {
        // Try fallback image
        Serial.println("Trying fallback image...");
        String url = String(SECRET_NEXTCLOUD_URL) + SECRET_NEXTCLOUD_FALLBACK;

        http.begin(url);
        http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
        http.setTimeout(NEXTCLOUD_TIMEOUT_MS);

        if (http.GET() == HTTP_CODE_OK) {
            imageSize = http.getSize();
            if (imageSize > 0 && imageSize < NEXTCLOUD_MAX_IMAGE_SIZE) {
                imageData = (uint8_t*)malloc(imageSize);
                if (imageData != NULL) {
                    WiFiClient* stream = http.getStreamPtr();
                    size_t totalRead = stream->readBytes(imageData, imageSize);
                    http.end();

                    if (totalRead == imageSize) {
                        displayImageOnEPaper(imageData, imageSize);
                        successfulUpdates++;
                    }
                    free(imageData);
                    return;
                }
            }
        }
        http.end();

        // Show error on display
        displayStatus("Image fetch failed", -1);
    }
}

// =============================================================================
// DEEP SLEEP
// =============================================================================

void goToSleep(uint32_t seconds) {
    Serial.printf("\n=== ENTERING DEEP SLEEP: %d seconds (%.1f hours) ===\n",
                  seconds, seconds / 3600.0);
    Serial.printf("Power state: %s\n", getPowerStateName(powerData.powerState));
    Serial.flush();

    // Final MQTT publish
    if (mqttConnected) {
        mqttClient.publish(MQTT_TOPIC_STATUS, "sleeping", true);
        mqttClient.loop();
        delay(200);
        mqttClient.disconnect();
        delay(100);
    }

    // Put display to sleep
    displaySleep();

    // Power off INA228
    powerOffINA228();

    // Turn off LED
    pinMode(LED_PIN, INPUT);

    // Disconnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Configure wake timer
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);

    bootTime = millis();
    bootCount++;

    // LED on during operation
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    Serial.println("\n========================================");
    Serial.printf("PhotonFrame v%s\n", FIRMWARE_VERSION);
    Serial.printf("Boot #%d | Reset: %s\n", bootCount, getResetReason());
    Serial.println("========================================");

    // 1. Power on and read INA228
    Serial.println("\n[1/6] Reading power metrics...");
    powerOnINA228();
    if (!initINA228()) {
        Serial.println("INA228 init failed, using defaults");
    }
    powerData = readAllPowerMetrics();

    // 2. Initialize display
    Serial.println("\n[2/6] Initializing display...");
    if (!initDisplay()) {
        logError("Display init failed");
    }

    // 3. Connect WiFi
    Serial.println("\n[3/7] Connecting to WiFi...");
    if (!connectWiFi()) {
        Serial.println("WiFi failed, sleeping...");
        goToSleep(WIFI_RETRY_SLEEP_SECONDS);
        return;
    }

    // 4. Arduino OTA window
    Serial.println("\n[4/7] Arduino OTA window...");
    setupArduinoOTA();
    runArduinoOTAWindow(ARDUINO_OTA_WINDOW_SECONDS);

    // 5. Check for Nextcloud/GitHub OTA updates
    Serial.println("\n[5/7] Checking for OTA updates...");
    if (wifiConnected) {
        if (nextcloudOTA.checkForUpdate()) {
            Serial.println("Nextcloud firmware found, updating...");
            nextcloudOTA.performUpdate();
        } else if (githubOTA.checkForUpdate()) {
            Serial.println("GitHub update available, updating...");
            githubOTA.performUpdate();
        } else {
            Serial.println("No updates available");
        }
    }

    // 6. Connect MQTT and publish discovery
    Serial.println("\n[6/7] Connecting to MQTT...");
    if (connectMQTT()) {
        publishHADiscovery();
        publishState();
    }

    // 7. Update display from Nextcloud
    Serial.println("\n[7/7] Updating display...");
    updateDisplay();

    // Final state publish
    if (mqttConnected) {
        publishState();
    }

    Serial.println("\n=== Setup complete, entering deep sleep ===");
    goToSleep(powerData.sleepSeconds);
}

// =============================================================================
// LOOP (handles OTA mode if activated via MQTT)
// =============================================================================

void loop() {
    // Handle Arduino OTA if in OTA mode
    if (otaModeActive) {
        ArduinoOTA.handle();
        mqttClient.loop();

        // Blink LED slowly to indicate OTA mode
        static unsigned long lastBlink = 0;
        if (millis() - lastBlink > 500) {
            lastBlink = millis();
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        }

        // Stay in OTA mode for up to 5 minutes, then sleep
        if (millis() - bootTime > 300000) {
            Serial.println("OTA mode timeout, sleeping...");
            otaModeActive = false;
            goToSleep(powerData.sleepSeconds);
        }
        return;
    }

    // Watchdog - should never reach here normally
    if (millis() - bootTime > WATCHDOG_TIMEOUT_MS) {
        Serial.println("WATCHDOG: Forcing sleep");
        goToSleep(powerData.sleepSeconds);
    }
    delay(1000);
}
