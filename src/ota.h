#ifndef OTA_H
#define OTA_H

#include "../secrets.h"
#include "config.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_system.h>

// Forward declarations
void logError(const char *error);

// OTA download timeout - abort if no data received for this long
#define OTA_STREAM_TIMEOUT_MS 30000

// Semantic version comparison: returns true if latest > current
static bool isNewerVersion(const String &current, const String &latest) {
  String cur = current.startsWith("v") ? current.substring(1) : current;
  String lat = latest.startsWith("v") ? latest.substring(1) : latest;

  int curParts[3] = {0, 0, 0};
  int latParts[3] = {0, 0, 0};

  int idx = 0;
  for (int i = 0; i < 3 && idx >= 0; i++) {
    int next = cur.indexOf('.', idx);
    curParts[i] =
        cur.substring(idx, next >= 0 ? next : cur.length()).toInt();
    idx = next >= 0 ? next + 1 : -1;
  }
  idx = 0;
  for (int i = 0; i < 3 && idx >= 0; i++) {
    int next = lat.indexOf('.', idx);
    latParts[i] =
        lat.substring(idx, next >= 0 ? next : lat.length()).toInt();
    idx = next >= 0 ? next + 1 : -1;
  }

  for (int i = 0; i < 3; i++) {
    if (latParts[i] > curParts[i])
      return true;
    if (latParts[i] < curParts[i])
      return false;
  }
  return false; // Equal versions
}

// =============================================================================
// NEXTCLOUD OTA UPDATE
// =============================================================================
// Check Nextcloud first - place firmware.bin in /Shared/firmware/photonframe/
// File is automatically deleted after successful update

class NextcloudOTA {
private:
  String firmwareUrl;

public:
  NextcloudOTA() {
    // Construct Nextcloud WebDAV URL for firmware
    String baseUrl = String(SECRET_NEXTCLOUD_URL);

    // Extract base server URL and construct firmware path
    int filesIndex = baseUrl.indexOf("/remote.php/dav/files/");
    if (filesIndex > 0) {
      int userStart = filesIndex + 22;
      int userEnd = baseUrl.indexOf("/", userStart);
      if (userEnd > 0) {
        firmwareUrl = baseUrl.substring(0, userEnd + 1) +
                      "Shared/firmware/photonframe/firmware.bin";
      }
    }

    // Fallback construction
    if (firmwareUrl.length() == 0) {
      int remoteIndex = baseUrl.indexOf("/remote.php");
      if (remoteIndex > 0) {
        firmwareUrl = baseUrl.substring(0, remoteIndex) +
                      "/remote.php/dav/files/" + String(SECRET_NEXTCLOUD_USER) +
                      "/Shared/firmware/photonframe/firmware.bin";
      }
    }

    Serial.printf("Nextcloud OTA URL: %s\n", firmwareUrl.c_str());
  }

  bool checkForUpdate() {
    if (firmwareUrl.length() == 0) {
      Serial.println("Nextcloud OTA: URL not configured");
      return false;
    }

    Serial.println("Nextcloud OTA: Checking for firmware...");

    HTTPClient http;
    http.begin(firmwareUrl);
    http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
    http.setTimeout(10000);

    int httpCode = http.sendRequest("HEAD");
    http.end();

    Serial.printf("Nextcloud OTA: HEAD response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Nextcloud OTA: Firmware found!");
      return true;
    } else if (httpCode == 404) {
      Serial.println("Nextcloud OTA: No firmware file (normal)");
    } else {
      Serial.printf("Nextcloud OTA: Check failed: %d\n", httpCode);
    }

    return false;
  }

  bool deleteFirmwareFile() {
    Serial.println("Nextcloud OTA: Deleting firmware file...");

    HTTPClient http;
    http.begin(firmwareUrl);
    http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
    http.setTimeout(10000);

    int httpCode = http.sendRequest("DELETE");
    http.end();

    if (httpCode == 204 || httpCode == 200) {
      Serial.println("Nextcloud OTA: File deleted");
      return true;
    }
    Serial.printf("Nextcloud OTA: Delete failed: %d\n", httpCode);
    return false;
  }

  bool performUpdate() {
    Serial.println("Nextcloud OTA: Downloading firmware...");

    HTTPClient http;
    http.begin(firmwareUrl);
    http.setAuthorization(SECRET_NEXTCLOUD_USER, SECRET_NEXTCLOUD_PASS);
    http.setTimeout(60000);

    int httpCode = http.GET();
    Serial.printf("Nextcloud OTA: HTTP response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      int contentLength = http.getSize();

      if (contentLength > 0 &&
          contentLength < (ESP.getFreeSketchSpace() - 0x1000)) {
        Serial.printf("Nextcloud OTA: Firmware size: %d bytes\n",
                      contentLength);

        WiFiClient *stream = http.getStreamPtr();

        if (Update.begin(contentLength, U_FLASH)) {
          Update.setMD5(
              nullptr); // Disable MD5 check to save memory if not provided

          size_t written = 0;
          size_t lastProgress = 0;
          uint8_t buffer[4096];
          bool ledState = false;
          unsigned long lastLedToggle = 0;
          unsigned long lastActivity = millis();

          while (written < contentLength) {
            yield(); // Feed watchdog

            if (millis() - lastActivity > OTA_STREAM_TIMEOUT_MS) {
              logError("Nextcloud OTA: Stream timeout");
              break;
            }

            // Blink LED during update
            if (millis() - lastLedToggle >= 100) {
              lastLedToggle = millis();
              ledState = !ledState;
              digitalWrite(LED_PIN, ledState ? HIGH : LOW);
            }

            size_t toRead =
                min((size_t)4096, (size_t)(contentLength - written));
            size_t bytesRead = stream->readBytes(buffer, toRead);

            if (bytesRead > 0) {
              if (Update.write(buffer, bytesRead) != bytesRead) {
                logError("Flash write failed");
                break;
              }
              written += bytesRead;
              lastActivity = millis();

              int progress = (written * 100) / contentLength;
              if (progress >= lastProgress + 5) {
                Serial.printf("Nextcloud OTA: %d%%\n", progress);
                lastProgress = progress;
              }
            } else {
              delay(10);
            }
          }

          if (written == contentLength) {
            Serial.println("Nextcloud OTA: Installing...");
            if (Update.end() && Update.isFinished()) {
              Serial.println("Nextcloud OTA: Success!");
              http.end();
              deleteFirmwareFile();
              Serial.println("Nextcloud OTA: Restarting...");
              Serial.flush();
              delay(2000);
              esp_restart();
              return true;
            }
            logError("Nextcloud OTA: Update.end() failed");
          } else {
            logError("Nextcloud OTA: Download incomplete");
          }
          Update.end();
        } else {
          logError("Nextcloud OTA: Update.begin() failed");
        }
      } else {
        logError("Nextcloud OTA: Invalid size or too large");
      }
    } else {
      logError("Nextcloud OTA: Download failed");
    }

    http.end();
    return false;
  }

};

// =============================================================================
// GITHUB OTA UPDATE (Fallback)
// =============================================================================

class GitHubOTA {
private:
  String currentVersion;
  String githubUser;
  String githubRepo;
  String githubTag;

public:
  GitHubOTA() {
    currentVersion = String(FIRMWARE_VERSION);
#ifdef SECRET_GITHUB_USER
    githubUser = String(SECRET_GITHUB_USER);
#else
    githubUser = String(OTA_GITHUB_USER);
#endif
#ifdef SECRET_GITHUB_REPO
    githubRepo = String(SECRET_GITHUB_REPO);
#else
    githubRepo = String(OTA_GITHUB_REPO);
#endif
    githubTag = String(OTA_GITHUB_TAG);
  }

  bool checkForUpdate() {
    Serial.println("GitHub OTA: Checking for updates...");

    String apiUrl = "https://api.github.com/repos/" + githubUser + "/" +
                    githubRepo + "/releases/latest";

    HTTPClient http;
    http.begin(apiUrl);
    http.setTimeout(10000);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      int tagIndex = payload.indexOf("\"tag_name\":\"");
      if (tagIndex > 0) {
        tagIndex += 12;
        int tagEnd = payload.indexOf("\"", tagIndex);
        String latestTag = payload.substring(tagIndex, tagEnd);

        String currentVer = currentVersion;
        String latestVer = latestTag;
        if (latestVer.startsWith("v"))
          latestVer = latestVer.substring(1);
        if (currentVer.startsWith("v"))
          currentVer = currentVer.substring(1);

        Serial.printf("GitHub OTA: Current: %s, Latest: %s\n",
                      currentVersion.c_str(), latestTag.c_str());

        if (latestVer.length() > 0 && isNewerVersion(currentVersion, latestTag)) {
          githubTag = latestTag;
          http.end();
          return true;
        }
      }
    }

    http.end();
    return false;
  }

  bool performUpdate() {
    String firmwareUrl = "https://github.com/" + githubUser + "/" + githubRepo +
                         "/releases/download/" + githubTag + "/" +
                         String(OTA_FIRMWARE_FILENAME);

    Serial.printf("GitHub OTA: Downloading from: %s\n", firmwareUrl.c_str());

    HTTPClient http;
    http.begin(firmwareUrl);
    http.setTimeout(60000); // uint16_t max is 65535 - don't overflow
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();
    Serial.printf("GitHub OTA: HTTP response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
        httpCode == HTTP_CODE_FOUND) {
      int contentLength = http.getSize();

      if (contentLength > 0 &&
          contentLength < (ESP.getFreeSketchSpace() - 0x1000)) {
        Serial.printf("GitHub OTA: Firmware size: %d bytes\n", contentLength);

        WiFiClient *stream = http.getStreamPtr();

        if (Update.begin(contentLength)) {
          size_t written = 0;
          size_t lastProgress = 0;
          uint8_t buffer[512];
          bool ledState = false;
          unsigned long lastLedToggle = 0;
          unsigned long lastActivity = millis();

          while (written < contentLength) {
            yield();

            if (millis() - lastActivity > OTA_STREAM_TIMEOUT_MS) {
              logError("GitHub OTA: Stream timeout");
              break;
            }

            if (millis() - lastLedToggle >= 100) {
              lastLedToggle = millis();
              ledState = !ledState;
              digitalWrite(LED_PIN, ledState ? HIGH : LOW);
            }

            size_t toRead = min((size_t)512, contentLength - written);
            size_t bytesRead = stream->readBytes(buffer, toRead);

            if (bytesRead > 0) {
              if (Update.write(buffer, bytesRead) != bytesRead) {
                logError("GitHub OTA: Flash write failed");
                break;
              }
              written += bytesRead;
              lastActivity = millis();

              int progress = (written * 100) / contentLength;
              if (progress >= lastProgress + 5) {
                Serial.printf("GitHub OTA: %d%%\n", progress);
                lastProgress = progress;
              }
            } else {
              delay(10);
            }
          }

          if (written == contentLength) {
            Serial.println("GitHub OTA: Installing...");
            if (Update.end() && Update.isFinished()) {
              Serial.println("GitHub OTA: Success! Restarting...");
              http.end();
              Serial.flush();
              delay(2000);
              esp_restart();
              return true;
            }
            logError("GitHub OTA: Update.end() failed");
          } else {
            logError("GitHub OTA: Download incomplete");
          }
          Update.end();
        } else {
          logError("GitHub OTA: Update.begin() failed");
        }
      } else {
        logError("GitHub OTA: Invalid size or too large");
      }
    } else {
      logError("GitHub OTA: HTTP request failed");
    }

    http.end();
    return false;
  }

};

#endif // OTA_H
