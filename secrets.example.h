/* SECRETS EXAMPLE - DO NOT COMMIT TO VERSION CONTROL
 * Copy this file to secrets.h and fill in your values.
 *
 * PhotonFrame - Indoor Solar E-Paper Display
 */

#ifndef SECRETS_H
#define SECRETS_H

// =============================================================================
// WIFI CREDENTIALS
// =============================================================================
// Primary credentials - used first before WiFiManager captive portal
#define SECRET_WIFI_SSID     "YourWiFiSSID"
#define SECRET_WIFI_PASSWORD "YourWiFiPassword"

// =============================================================================
// MQTT CONFIGURATION (for Home Assistant)
// =============================================================================
#define SECRET_MQTT_BROKER    "192.168.1.100"      // Your MQTT broker IP
#define SECRET_MQTT_PORT      1883                  // Default MQTT port
#define SECRET_MQTT_USERNAME  "mqtt_username"       // Leave empty if no auth
#define SECRET_MQTT_PASSWORD  "mqtt_password"       // Leave empty if no auth
#define SECRET_MQTT_CLIENT_ID "photonframe_001"     // Unique client ID

// =============================================================================
// NEXTCLOUD WEBDAV (for image fetching)
// =============================================================================
// Base URL should end with the folder containing your images
// Example: https://cloud.example.com/remote.php/dav/files/Username/Photos/EPaper/
#define SECRET_NEXTCLOUD_URL      "https://your-nextcloud.com/remote.php/dav/files/Username/Photos/EPaper/"
#define SECRET_NEXTCLOUD_USER     "YourNextcloudUsername"
#define SECRET_NEXTCLOUD_PASS     "YourNextcloudAppPassword"  // Use app password, not main password

// Image filenames in the Nextcloud folder
#define SECRET_NEXTCLOUD_PHOTO    "display.png"     // Primary image
#define SECRET_NEXTCLOUD_FALLBACK "fallback.png"    // Fallback if primary fails

// =============================================================================
// OTA UPDATE CONFIGURATION
// =============================================================================
// GitHub repository for OTA updates (optional)
// Format: username/repo
#define SECRET_GITHUB_USER "YourGitHubUsername"
#define SECRET_GITHUB_REPO "PhotonFrame"

#endif // SECRETS_H
