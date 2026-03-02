/*
 * PhotonFrame - Indoor Solar E-Paper Display
 *
 * Features:
 * - INA228 solar power monitoring with hybrid refresh scheduling
 * - 7.3" Spectra 6 (7-color) e-paper display
 * - Nextcloud WebDAV image fetching
 * - MQTT integration with Home Assistant autodiscovery
 * - Dual OTA updates (Nextcloud priority, GitHub fallback)
 */

#include "../secrets.h"
#include "config.h"
#include "display.h"
#include "ota.h"
#include "power.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
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
unsigned long bootTime = 0;
PowerReadings powerData = {0};

// Error tracking
uint32_t errorCount = 0;
String lastError = "";

// RTC memory (survives deep sleep)
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t totalErrorCount = 0;
RTC_DATA_ATTR uint32_t successfulUpdates = 0;
RTC_DATA_ATTR bool haDiscoveryPublished = false;
RTC_DATA_ATTR uint32_t discoveryVersion = 0; // Increment to force republish
RTC_DATA_ATTR char lastImageETag[64] = {0};  // ETag cache to skip unchanged images

// Current discovery config version - increment when changing discovery format
#define HA_DISCOVERY_VERSION 5

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

void logError(const char *error) {
  errorCount++;
  totalErrorCount++;
  lastError = String(error);
  Serial.printf("ERROR [%d]: %s\n", errorCount, error);
}

const char *getResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
  case ESP_RST_POWERON:
    return "Power on";
  case ESP_RST_SW:
    return "Software reset";
  case ESP_RST_PANIC:
    return "Exception/Panic";
  case ESP_RST_DEEPSLEEP:
    return "Deep sleep wake";
  case ESP_RST_BROWNOUT:
    return "Brownout";
  default:
    return "Unknown";
  }
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
    WiFi.setSleep(true); // Enable modem sleep between beacons (~30% power saving)
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  // Only start captive portal on power-on or manual reset (not deep sleep wakes).
  // Running AP mode burns ~120mA - acceptable for initial setup, not every 4h cycle.
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_DEEPSLEEP) {
    Serial.println("WiFi failed, starting captive portal (first boot)...");
    wifiManager.setConfigPortalTimeout(180);
    if (wifiManager.autoConnect("PhotonFrame-Setup")) {
      wifiConnected = true;
      WiFi.setSleep(true);
      Serial.printf("WiFi connected via portal: %s\n",
                    WiFi.localIP().toString().c_str());
      return true;
    }
  }

  Serial.println("WiFi failed");
  wifiConnected = false;
  return false;
}

// =============================================================================
// MQTT FUNCTIONS
// =============================================================================

// MQTT commands only work during the brief wake window (~30s).
// Use HA automation triggered by photonframe/availability -> "online" to send commands.
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.printf("MQTT: %s -> %s\n", topic, message.c_str());

  if (String(topic) == MQTT_TOPIC_COMMAND) {
    JsonDocument doc;
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
        if (nextcloudOTA.checkForUpdate()) {
          nextcloudOTA.performUpdate();
        } else if (githubOTA.checkForUpdate()) {
          githubOTA.performUpdate();
        }
      } else if (action == "clear_errors") {
        errorCount = 0;
        totalErrorCount = 0;
        lastError = "";
        Serial.println("Errors cleared");
      } else if (action == "force_ha_discovery") {
        haDiscoveryPublished = false;
        Serial.println("HA discovery will republish");
      }
    }
  }
}

bool connectMQTT() {
  if (!wifiConnected)
    return false;

  Serial.printf("Connecting to MQTT: %s:%d\n", SECRET_MQTT_BROKER,
                SECRET_MQTT_PORT);

  mqttClient.setServer(SECRET_MQTT_BROKER, SECRET_MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);

  // Connect with LWT (Last Will Testament) - QoS 1, retained
  if (mqttClient.connect(SECRET_MQTT_CLIENT_ID, SECRET_MQTT_USERNAME,
                         SECRET_MQTT_PASSWORD, MQTT_TOPIC_AVAILABILITY, 1, true,
                         "offline")) {
    Serial.println("MQTT connected");
    mqttConnected = true;

    // Immediately publish online status (retained, QoS 1)
    mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
    mqttClient.loop();

    mqttClient.subscribe(MQTT_TOPIC_COMMAND, 1);
    return true;
  }

  Serial.printf("MQTT failed: %d\n", mqttClient.state());
  mqttConnected = false;
  return false;
}

void publishHADiscovery() {
  if (!mqttConnected)
    return;

  // Check if we need to republish (version changed or first boot)
  if (haDiscoveryPublished && discoveryVersion == HA_DISCOVERY_VERSION &&
      bootCount > 1) {
    Serial.println("HA discovery already published (v" +
                   String(HA_DISCOVERY_VERSION) + ")");
    return;
  }

  Serial.println("Publishing HA autodiscovery...");

  // Device info for all sensors
  auto addDevice = [](JsonDocument &doc) {
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = DEVICE_ID;
    device["name"] = DEVICE_NAME;
    device["manufacturer"] = DEVICE_MANUFACTURER;
    device["model"] = DEVICE_MODEL;
    device["sw_version"] = FIRMWARE_VERSION;
  };

  // Sensor definitions
  struct SensorDef {
    const char *id;
    const char *name;
    const char *valueTemplate;
    const char *unit;
    const char *deviceClass;
    const char *stateClass;
    bool isBinary;
  };

  SensorDef sensors[] = {
      {"battery", "Battery", "{{ value_json.battery_percentage }}", "%",
       "battery", "measurement", false},
      {"voltage", "Battery Voltage", "{{ value_json.battery_voltage }}", "V",
       "voltage", "measurement", false},
      // Solar sensors use non-standard units - no device_class to avoid HA
      // validation errors
      {"solar_voltage", "Solar Voltage", "{{ value_json.solar_voltage_mV }}",
       "mV", nullptr, "measurement", false},
      {"solar_current", "Solar Current", "{{ value_json.solar_current_uA }}",
       "uA", nullptr, "measurement", false},
      {"solar_power", "Solar Power", "{{ value_json.solar_power_uW }}", "uW",
       nullptr, "measurement", false},
      {"ina_temp", "INA Temperature", "{{ value_json.ina_temperature }}", "C",
       "temperature", "measurement", false},
      {"power_state", "Power State", "{{ value_json.power_state }}", nullptr,
       nullptr, nullptr, false},
      {"next_refresh", "Next Refresh", "{{ value_json.next_refresh_seconds }}",
       "s", "duration", "measurement", false},
      {"wifi_rssi", "WiFi Signal", "{{ value_json.wifi_rssi }}", "dBm",
       "signal_strength", "measurement", false},
      {"boot_count", "Boot Count", "{{ value_json.boot_count }}", nullptr,
       nullptr, "total_increasing", false},
      {"version", "Firmware", "{{ value_json.firmware_version }}", nullptr,
       nullptr, nullptr, false},
  };

  for (auto &sensor : sensors) {
    JsonDocument doc;
    doc["unique_id"] = String(DEVICE_ID) + "_" + sensor.id;
    doc["name"] = String(DEVICE_NAME) + " " + sensor.name;
    doc["state_topic"] = MQTT_TOPIC_STATE;
    doc["value_template"] = sensor.valueTemplate;

    // For deep-sleep devices: don't use availability_topic, use expire_after
    // instead State expires after 25 hours (max sleep is 24h)
    doc["expire_after"] = 90000;

    if (sensor.unit)
      doc["unit_of_measurement"] = sensor.unit;
    if (sensor.deviceClass)
      doc["device_class"] = sensor.deviceClass;
    if (sensor.stateClass)
      doc["state_class"] = sensor.stateClass;

    addDevice(doc);

    String configTopic = "homeassistant/sensor/" + String(DEVICE_ID) + "_" +
                         sensor.id + "/config";
    String payload;
    serializeJson(doc, payload);

    Serial.printf("  Registering: %s\n", sensor.name);
    if (!mqttClient.publish(configTopic.c_str(), payload.c_str(), true)) {
      Serial.printf("  FAILED to register %s\n", sensor.name);
    }
    mqttClient.loop();
    delay(10);
  }

  Serial.println("Finalizing discovery...");
  mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
  haDiscoveryPublished = true;
  discoveryVersion = HA_DISCOVERY_VERSION;

  // Flush MQTT
  for (int i = 0; i < 5; i++) {
    mqttClient.loop();
    delay(50);
  }

  Serial.printf("HA discovery published (v%d)\n", HA_DISCOVERY_VERSION);
}

void publishState() {
  if (!mqttConnected) {
    if (!connectMQTT())
      return;
  }

  JsonDocument doc;

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
// NEXTCLOUD IMAGE FETCH (with LittleFS caching to avoid RAM fragmentation)
// =============================================================================

#define TEMP_IMAGE_PATH "/temp_image.png"

bool initFileSystem() {
  if (!LittleFS.begin(true)) { // true = format if failed
    Serial.println("LittleFS mount failed");
    return false;
  }
  Serial.printf("LittleFS: %d bytes used, %d bytes total\n",
                LittleFS.usedBytes(), LittleFS.totalBytes());
  return true;
}

bool downloadImageToFile(const String &url) {
  Serial.printf("Downloading to flash: %s\n", url.c_str());
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  http.end();

  http.begin(url);
  http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
  http.setTimeout(NEXTCLOUD_TIMEOUT_MS);
  http.setReuse(false);

  // Collect ETag header for change detection
  const char *headerKeys[] = {"ETag"};
  http.collectHeaders(headerKeys, 1);

  int httpCode = http.GET();
  Serial.printf("HTTP response: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP error: %d", httpCode);
    logError(buf);
    http.end();
    return false;
  }

  size_t imageSize = http.getSize();
  Serial.printf("Image size: %d bytes\n", imageSize);

  if (imageSize == 0 || imageSize >= NEXTCLOUD_MAX_IMAGE_SIZE) {
    logError("Invalid image size");
    http.end();
    return false;
  }

  // Open file for writing
  File file = LittleFS.open(TEMP_IMAGE_PATH, "w");
  if (!file) {
    logError("Failed to open file for writing");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (stream == NULL) {
    logError("HTTP stream null");
    file.close();
    http.end();
    return false;
  }

  // Download to file in chunks (only need small buffer)
  uint8_t buffer[1024];
  size_t totalWritten = 0;
  unsigned long lastActivity = millis();

  while (totalWritten < imageSize) {
    if (millis() - lastActivity > 30000) {
      logError("Download timeout");
      file.close();
      LittleFS.remove(TEMP_IMAGE_PATH);
      http.end();
      return false;
    }

    int available = stream->available();
    if (available == 0) {
      delay(10);
      continue;
    }

    size_t toRead = min((size_t)available, sizeof(buffer));
    toRead = min(toRead, imageSize - totalWritten);
    size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead > 0) {
      size_t written = file.write(buffer, bytesRead);
      if (written != bytesRead) {
        logError("File write failed");
        file.close();
        LittleFS.remove(TEMP_IMAGE_PATH);
        http.end();
        return false;
      }
      totalWritten += written;
      lastActivity = millis();

      // Progress every 20KB
      if (totalWritten % 20480 < 1024) {
        Serial.printf("Downloading: %d/%d bytes (%.0f%%)\n", totalWritten,
                      imageSize, (totalWritten * 100.0) / imageSize);
      }
    }
  }

  file.close();

  // Cache ETag for future change detection
  String etag = http.header("ETag");
  if (etag.length() > 0 && etag.length() < sizeof(lastImageETag) - 1) {
    strncpy(lastImageETag, etag.c_str(), sizeof(lastImageETag) - 1);
    lastImageETag[sizeof(lastImageETag) - 1] = '\0';
    Serial.printf("Cached ETag: %s\n", lastImageETag);
  }

  http.end();

  Serial.printf("Download complete: %d bytes saved to flash\n", totalWritten);
  return totalWritten == imageSize;
}

bool displayImageFromFile() {
  Serial.printf("Displaying PNG from file, free heap: %d bytes\n",
                ESP.getFreeHeap());

  // Stream directly from file to PNG decoder (never loads entire PNG to RAM)
  bool success = displayPNGFromFile(TEMP_IMAGE_PATH);

  // Clean up temp file
  LittleFS.remove(TEMP_IMAGE_PATH);

  if (success) {
    Serial.println("Image displayed successfully");
  } else {
    logError("PNG decode failed");
  }

  return success;
}

bool fetchAndDisplayImageFromNextcloud(const String &photoPath) {
  if (!wifiConnected) {
    logError("WiFi not connected");
    return false;
  }

  String url = String(SECRET_NEXTCLOUD_URL) + photoPath;

  // Download to flash first (uses minimal RAM)
  if (!downloadImageToFile(url)) {
    return false;
  }

  // Display from flash (HTTP closed, more RAM available for buffer)
  return displayImageFromFile();
}

void updateDisplay() {
  Serial.println("Updating display from Nextcloud...");

  String primaryUrl = String(SECRET_NEXTCLOUD_URL) + SECRET_NEXTCLOUD_PHOTO;

  // Quick HEAD request to check if image has changed (saves ~40s if unchanged)
  if (lastImageETag[0] != '\0') {
    HTTPClient headCheck;
    headCheck.begin(primaryUrl);
    headCheck.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
    headCheck.setTimeout(10000);
    const char *keys[] = {"ETag"};
    headCheck.collectHeaders(keys, 1);

    int code = headCheck.sendRequest("HEAD");
    if (code == HTTP_CODE_OK) {
      String etag = headCheck.header("ETag");
      headCheck.end();
      if (etag.length() > 0 && strcmp(lastImageETag, etag.c_str()) == 0) {
        Serial.println("Image unchanged (ETag match), skipping display refresh");
        successfulUpdates++;
        return;
      }
      Serial.printf("Image changed (old ETag != new), will refresh\n");
    } else {
      headCheck.end();
      Serial.printf("HEAD check returned %d, proceeding with download\n", code);
    }
  }

  // Try primary image (streaming - no large buffer needed)
  if (fetchAndDisplayImageFromNextcloud(SECRET_NEXTCLOUD_PHOTO)) {
    successfulUpdates++;
    return;
  }

  // Try fallback image (also streaming)
  Serial.println("Trying fallback image...");
  if (fetchAndDisplayImageFromNextcloud(SECRET_NEXTCLOUD_FALLBACK)) {
    successfulUpdates++;
    return;
  }

  // Show error on display
  displayStatus("Image fetch failed", -1);
}

// =============================================================================
// DEEP SLEEP
// =============================================================================

// =============================================================================
// EMERGENCY SHUTDOWN - Battery dangerously low
// =============================================================================
// This function puts the device into indefinite hibernation.
// It will NOT wake on a timer - only manual reset or charging will wake it.
// This protects the LiPo from deep discharge damage.

void emergencyShutdown(float voltage) {
  Serial.printf("\n!!! EMERGENCY SHUTDOWN !!!\n");
  Serial.printf("Battery voltage: %.2fV (threshold: %.2fV)\n", voltage,
                VOLTAGE_SHUTDOWN);
  Serial.printf("Device will hibernate for 7 days to protect battery.\n");
  Serial.flush();

  // Disconnect WiFi if it was started
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Put display into low-power state (SPI pins, RTC holds)
  // Safe to call even if display wasn't initialized - just configures GPIOs
  displaySleep();

  // Cut INA228 power and isolate I2C pins
  powerOffINA228();

  // LED off with RTC hold
  digitalWrite(LED_PIN, LOW);
  rtc_gpio_init((gpio_num_t)LED_PIN);
  rtc_gpio_set_direction((gpio_num_t)LED_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)LED_PIN, 0);
  rtc_gpio_hold_en((gpio_num_t)LED_PIN);

  // Enable deep sleep hold for ALL RTC GPIOs
  gpio_deep_sleep_hold_en();

  // Sleep for 7 days
  const uint64_t SEVEN_DAYS_US = 7ULL * 24 * 60 * 60 * 1000000ULL;
  esp_sleep_enable_timer_wakeup(SEVEN_DAYS_US);
  esp_deep_sleep_start();
}

void goToSleep(uint32_t seconds) {
  Serial.printf("\n=== ENTERING DEEP SLEEP: %d seconds (%.1f hours) ===\n",
                seconds, seconds / 3600.0);
  Serial.printf("Power state: %s\n", getPowerStateName(powerData.powerState));
  Serial.flush();

  // Final MQTT publish - flush multiple times to ensure delivery
  if (mqttConnected) {
    mqttClient.publish(MQTT_TOPIC_STATUS, "sleeping", true);

    // Flush MQTT buffer
    for (int i = 0; i < 5; i++) {
      mqttClient.loop();
      delay(50);
    }

    mqttClient.disconnect();
    delay(100);
  }

  // Disconnect WiFi fully before configuring GPIOs
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Put display to sleep (configures all display RTC GPIO holds)
  displaySleep();

  // Power off INA228 (configures INA228 power pin RTC hold)
  powerOffINA228();

  // Turn off LED (GPIO 2 is RTC-capable) - hold LOW during sleep
  digitalWrite(LED_PIN, LOW);
  rtc_gpio_init((gpio_num_t)LED_PIN);
  rtc_gpio_set_direction((gpio_num_t)LED_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)LED_PIN, 0);
  rtc_gpio_hold_en((gpio_num_t)LED_PIN);

  // Enable deep sleep hold for ALL RTC GPIOs - must be after all rtc_gpio_hold_en() calls
  gpio_deep_sleep_hold_en();

  // Configure wake timer (minimum 1 hour guard prevents boot loops from zero sleep)
  if (seconds < 3600) seconds = 3600;
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// =============================================================================
// SETUP
// =============================================================================

// Forward declaration for emergency shutdown
void emergencyShutdown(float voltage);

void setup() {
  Serial.begin(115200);
  delay(100);

  bootTime = millis();
  bootCount++;

  // Release LED GPIO hold from sleep, then turn on during operation
  rtc_gpio_hold_dis((gpio_num_t)LED_PIN);
  rtc_gpio_deinit((gpio_num_t)LED_PIN);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.println("\n========================================");
  Serial.printf("PhotonFrame v%s\n", FIRMWARE_VERSION);
  Serial.printf("Boot #%d | Reset: %s\n", bootCount, getResetReason());
  Serial.println("========================================");
  Serial.printf("Free heap at start: %d bytes\n", ESP.getFreeHeap());

  // ==========================================================================
  // CRITICAL: IMMEDIATE BATTERY CHECK - before ANY power-hungry operations!
  // ==========================================================================
  // Read battery voltage directly (no INA228 needed, uses ESP32 ADC)
  float immediateVoltage = 0;
  for (int i = 0; i < 5; i++) {
    int adcValue = analogRead(BAT_ADC_PIN);
    immediateVoltage += adcValue * BAT_ADC_MULTIPLIER * BAT_ADC_VREF / 4095.0f;
    delay(10);
  }
  immediateVoltage /= 5.0f;

  Serial.printf("BATTERY CHECK: %.2fV\n", immediateVoltage);

  // HARD SHUTDOWN: Battery dangerously low - hibernate indefinitely
  if (immediateVoltage < VOLTAGE_SHUTDOWN) {
    emergencyShutdown(immediateVoltage);
    return; // Never reached
  }

  // CRITICAL: Skip all non-essential operations, sleep immediately
  if (immediateVoltage < VOLTAGE_CRITICAL) {
    Serial.printf(
        "CRITICAL BATTERY: %.2fV - skipping all operations, sleeping 24h\n",
        immediateVoltage);

    // Configure all pins for lowest power even though peripherals aren't initialized
    displaySleep();
    powerOffINA228();

    digitalWrite(LED_PIN, LOW);
    rtc_gpio_init((gpio_num_t)LED_PIN);
    rtc_gpio_set_direction((gpio_num_t)LED_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level((gpio_num_t)LED_PIN, 0);
    rtc_gpio_hold_en((gpio_num_t)LED_PIN);
    gpio_deep_sleep_hold_en();

    // Sleep for 24 hours
    esp_sleep_enable_timer_wakeup(SLEEP_EMERGENCY_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
    return; // Never reached
  }

  // Initialize LittleFS for image caching
  if (!initFileSystem()) {
    Serial.println("WARNING: LittleFS init failed, image caching disabled");
  }

  // 1. Power on and read INA228
  Serial.println("\n[1/6] Reading power metrics...");
  powerOnINA228();
  if (!initINA228()) {
    Serial.println("INA228 init failed, using defaults");
  }
  powerData = readAllPowerMetrics();

  // Re-check battery after full reading (more accurate with INA228)
  if (powerData.batteryVoltage < VOLTAGE_SHUTDOWN) {
    emergencyShutdown(powerData.batteryVoltage);
    return;
  }

  // 2. Initialize display
  Serial.println("\n[2/6] Initializing display...");
  if (!initDisplay()) {
    logError("Display init failed");
  }

  // 3. Connect WiFi
  Serial.println("\n[3/6] Connecting to WiFi...");
  if (!connectWiFi()) {
    Serial.println("WiFi failed, sleeping...");
    goToSleep(WIFI_RETRY_SLEEP_SECONDS);
    return;
  }

  // 4. Check for Nextcloud/GitHub OTA updates
  Serial.println("\n[4/6] Checking for OTA updates...");
  // Only check OTA every Nth boot to save 2 HTTPS handshakes (~5-10s at ~200mA)
  bool otaCheckDue = (bootCount % OTA_CHECK_INTERVAL == 1);
  // Skip OTA if last reset was a panic - prevents bootloop from bad OTA
  bool lastResetWasPanic = (esp_reset_reason() == ESP_RST_PANIC);
  if (!otaCheckDue && !lastResetWasPanic) {
    Serial.printf("Skipping OTA check (boot %d, next check at boot %d)\n",
                  bootCount, ((bootCount / OTA_CHECK_INTERVAL) + 1) * OTA_CHECK_INTERVAL + 1);
  } else if (lastResetWasPanic) {
    Serial.println("Skipping OTA - last reset was panic (preventing bootloop)");
    // Try to delete the Nextcloud firmware to prevent future crash
    if (wifiConnected) {
      nextcloudOTA.deleteFirmwareFile();
    }
  } else if (powerData.batteryVoltage < VOLTAGE_LOW) {
    // Skip OTA if battery is low - OTA is power-hungry and risky with low
    // battery
    Serial.printf("Skipping OTA check - battery low (%.2fV)\n",
                  powerData.batteryVoltage);
  } else if (wifiConnected) {
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
  Serial.println("\n[5/6] Connecting to MQTT...");
  if (connectMQTT()) {
    publishHADiscovery();
    publishState();
  }

  // 7. Update display from Nextcloud
  Serial.println("\n[6/6] Updating display...");
  // Skip display update if battery is too low - display refresh uses
  // significant power
  if (powerData.batteryVoltage < VOLTAGE_LOW) {
    Serial.printf("Skipping display update - battery low (%.2fV)\n",
                  powerData.batteryVoltage);
  } else {
    updateDisplay();
  }

  // Final state publish
  if (mqttConnected) {
    publishState();
  }

  Serial.println("\n=== Setup complete, entering deep sleep ===");
  goToSleep(powerData.sleepSeconds);
}

// =============================================================================
// LOOP (watchdog only - device should sleep before reaching here)
// =============================================================================

void loop() {
  // Watchdog - should never reach here normally
  if (millis() - bootTime > WATCHDOG_TIMEOUT_MS) {
    Serial.println("WATCHDOG: Forcing sleep");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    uint32_t sleepSec = powerData.sleepSeconds > 0 ? powerData.sleepSeconds : SLEEP_DEFAULT_SECONDS;
    goToSleep(sleepSec);
  }
  delay(1000);
}
