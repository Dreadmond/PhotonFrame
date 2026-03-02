#include "display.h"
#include "config.h"
#include "pngle.h"
#include <Adafruit_GFX.h>
#include <LittleFS.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

// =============================================================================
// DISPLAY OBJECT
// =============================================================================
DisplayType display(GxEPD2_730c_GDEP073E01(
    /*CS=*/EPD_CS_PIN,
    /*DC=*/EPD_DC_PIN,
    /*RST=*/EPD_RST_PIN,
    /*BUSY=*/EPD_BUSY_PIN));

// =============================================================================
// MODULE STATE
// =============================================================================
static bool displayInitialized = false;

// PNG decoder state
static uint32_t pngWidth = 0;
static uint32_t pngHeight = 0;
static bool pngDrawing = false;
static uint8_t *pngDataPtr = NULL;
static size_t pngDataSize = 0;

// =============================================================================
// COLOR QUANTIZATION
// =============================================================================
// Convert RGB to 7-color Spectra 6 palette using weighted Euclidean distance
// Returns GxEPD color constant

uint16_t rgbTo7Color(uint8_t r, uint8_t g, uint8_t b) {
  struct PaletteEntry {
    uint8_t r, g, b;
    uint16_t color;
  };

  // Spectra 6 actual color palette (7 colors)
  static const PaletteEntry palette[] = {
      {0, 0, 0, GxEPD_BLACK},      {255, 255, 255, GxEPD_WHITE},
      {200, 0, 0, GxEPD_RED},      {255, 255, 0, GxEPD_YELLOW},
      {255, 128, 0, GxEPD_ORANGE}, {0, 180, 0, GxEPD_GREEN},
      {0, 0, 200, GxEPD_BLUE},
  };

  uint32_t bestDistance = UINT32_MAX;
  uint16_t bestColor = GxEPD_WHITE;

  for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
    const PaletteEntry &entry = palette[i];

    // Weighted Euclidean distance (human eye sensitivity)
    int dr = (int)r - (int)entry.r;
    int dg = (int)g - (int)entry.g;
    int db = (int)b - (int)entry.b;

    // R: 0.299, G: 0.587, B: 0.114 (scaled up)
    uint32_t dist = (dr * dr * 2) + (dg * dg * 4) + (db * db);

    if (dist < bestDistance) {
      bestDistance = dist;
      bestColor = entry.color;
    }
  }

  return bestColor;
}

// =============================================================================
// PNG DECODER CALLBACKS (for pngle library)
// =============================================================================

void onPngInit(pngle_t *pngle, uint32_t w, uint32_t h) {
  pngWidth = w;
  pngHeight = h;
  Serial.printf("PNG init: %dx%d\n", w, h);
}

void onPngDraw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               const uint8_t rgba[4]) {
  if (!pngDrawing || x >= pngWidth || y >= pngHeight)
    return;

  // Center the image on display
  int offsetX = (DISPLAY_WIDTH - pngWidth) / 2;
  int offsetY = (DISPLAY_HEIGHT - pngHeight) / 2;
  if (offsetX < 0)
    offsetX = 0;
  if (offsetY < 0)
    offsetY = 0;

  uint16_t color = rgbTo7Color(rgba[0], rgba[1], rgba[2]);

  int drawX = offsetX + x;
  int drawY = offsetY + y;

  if (drawX >= 0 && drawX < DISPLAY_WIDTH && drawY >= 0 &&
      drawY < DISPLAY_HEIGHT) {
    display.drawPixel(drawX, drawY, color);
  }
}

void onPngDone(pngle_t *pngle) { Serial.println("PNG decode complete"); }

// =============================================================================
// PNG DECODING
// =============================================================================

bool decodeAndDisplayPNG(uint8_t *pngData, size_t pngSize) {
  Serial.println("Decoding PNG with pngle...");
  Serial.printf("PNG data size: %d bytes\n", pngSize);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  // Verify PNG signature
  if (pngSize < 8 || pngData[0] != 0x89 || pngData[1] != 0x50 ||
      pngData[2] != 0x4E || pngData[3] != 0x47) {
    Serial.println("ERROR: Invalid PNG header");
    return false;
  }

  display.setRotation(0);
  display.setFullWindow();

  // Reset state
  pngWidth = 0;
  pngHeight = 0;
  pngDrawing = false;
  pngDataPtr = pngData;
  pngDataSize = pngSize;

  bool decodeSuccess = false;

  // Paging loop - decode PNG on every page pass
  display.firstPage();
  do {
    display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);

    pngle_t *pngle = pngle_new();
    if (pngle == NULL) {
      Serial.println("ERROR: Failed to create pngle decoder");
      Serial.printf("Free heap when pngle_new failed: %d bytes\n",
                    ESP.getFreeHeap());
      continue;
    }

    pngle_set_init_callback(pngle, onPngInit);
    pngle_set_draw_callback(pngle, onPngDraw);
    pngle_set_done_callback(pngle, onPngDone);

    pngDrawing = true;

    // Feed PNG in chunks to reduce peak memory
    size_t offset = 0;
    const size_t chunkSize = 4096;
    int result = 0;

    while (offset < pngDataSize) {
      size_t toFeed = (pngDataSize - offset < chunkSize)
                          ? (pngDataSize - offset)
                          : chunkSize;
      result = pngle_feed(pngle, pngDataPtr + offset, toFeed);
      if (result < 0) {
        const char *error = pngle_error(pngle);
        Serial.printf("ERROR: pngle_feed failed at %d: %s\n", offset,
                      error ? error : "unknown");
        break;
      }
      offset += toFeed;
    }

    pngDrawing = false;
    pngle_destroy(pngle);

    if (result >= 0 && pngWidth > 0 && pngHeight > 0) {
      decodeSuccess = true;
    }
  } while (display.nextPage());

  pngDataPtr = NULL;
  pngDataSize = 0;

  if (decodeSuccess) {
    Serial.printf("PNG rendered: %dx%d\n", pngWidth, pngHeight);
    return true;
  }

  Serial.println("ERROR: PNG decode failed");
  return false;
}

// =============================================================================
// FILE-STREAMING PNG DECODE (Memory efficient - never loads entire PNG to RAM)
// =============================================================================

bool displayPNGFromFile(const char *filePath) {
  Serial.printf("Streaming PNG from file: %s\n", filePath);
  Serial.printf("Free heap before: %d bytes\n", ESP.getFreeHeap());

  File file = LittleFS.open(filePath, "r");
  if (!file) {
    Serial.println("ERROR: Failed to open PNG file");
    return false;
  }

  size_t fileSize = file.size();
  Serial.printf("File size: %d bytes\n", fileSize);

  // Read and verify PNG signature
  uint8_t header[8];
  if (file.read(header, 8) != 8) {
    Serial.println("ERROR: Failed to read PNG header");
    file.close();
    return false;
  }

  if (header[0] != 0x89 || header[1] != 0x50 || header[2] != 0x4E ||
      header[3] != 0x47) {
    Serial.println("ERROR: Invalid PNG signature");
    file.close();
    return false;
  }

  display.setRotation(0);
  display.setFullWindow();

  // Reset state
  pngWidth = 0;
  pngHeight = 0;

  bool decodeSuccess = false;

  // Small chunk buffer - only 2KB needed at a time
  const size_t chunkSize = 2048;
  uint8_t *chunk = (uint8_t *)malloc(chunkSize);
  if (chunk == NULL) {
    Serial.println("ERROR: Failed to allocate chunk buffer");
    file.close();
    return false;
  }

  Serial.printf("Free heap after chunk alloc: %d bytes\n", ESP.getFreeHeap());

  // Paging loop - decode PNG on every page pass
  display.firstPage();
  do {
    display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);

    // Seek back to start of file for each page
    file.seek(0);

    pngle_t *pngle = pngle_new();
    if (pngle == NULL) {
      Serial.printf("ERROR: pngle_new failed, heap: %d\n", ESP.getFreeHeap());
      continue;
    }

    pngle_set_init_callback(pngle, onPngInit);
    pngle_set_draw_callback(pngle, onPngDraw);
    pngle_set_done_callback(pngle, onPngDone);

    pngDrawing = true;

    // Stream file to pngle in chunks
    size_t totalFed = 0;
    int result = 0;

    while (totalFed < fileSize && result >= 0) {
      size_t toRead = min(chunkSize, fileSize - totalFed);
      size_t bytesRead = file.read(chunk, toRead);

      if (bytesRead == 0) {
        Serial.println("ERROR: File read returned 0");
        break;
      }

      result = pngle_feed(pngle, chunk, bytesRead);
      if (result < 0) {
        const char *error = pngle_error(pngle);
        Serial.printf("ERROR: pngle_feed failed: %s\n",
                      error ? error : "unknown");
        break;
      }

      totalFed += bytesRead;
    }

    pngDrawing = false;
    pngle_destroy(pngle);

    if (result >= 0 && pngWidth > 0 && pngHeight > 0) {
      decodeSuccess = true;
      Serial.printf("Page decoded: %dx%d\n", pngWidth, pngHeight);
    }
  } while (display.nextPage());

  free(chunk);
  file.close();

  Serial.printf("Free heap after: %d bytes\n", ESP.getFreeHeap());

  if (decodeSuccess) {
    Serial.printf("PNG rendered from file: %dx%d\n", pngWidth, pngHeight);
    return true;
  }

  Serial.println("ERROR: PNG file decode failed");
  return false;
}

// =============================================================================
// DISPLAY INITIALIZATION
// =============================================================================

bool initDisplay() {
  if (displayInitialized) {
    Serial.println("Display already initialized");
    return true;
  }

  // Release GPIO holds from sleep
  displayWake();

  Serial.println("Initializing SPI...");
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, EPD_CS_PIN);
  delay(100);

  Serial.println("Initializing display driver...");
  display.init(115200, true); // true = reset display
  delay(100);

  display.setRotation(0);

  displayInitialized = true;
  Serial.println("Display initialized");
  return true;
}

void resetDisplayInit() { displayInitialized = false; }

// =============================================================================
// IMAGE DISPLAY
// =============================================================================

void displayImageOnEPaper(uint8_t *imageData, size_t imageSize) {
  Serial.printf("Displaying image: %d bytes\n", imageSize);

  if (imageData == NULL || imageSize == 0) {
    Serial.println("ERROR: Invalid image data");
    clearDisplay();
    return;
  }

  // Check for PNG format
  bool isPNG =
      (imageSize >= 8 && imageData[0] == 0x89 && imageData[1] == 0x50 &&
       imageData[2] == 0x4E && imageData[3] == 0x47);

  if (isPNG) {
    Serial.println("Detected PNG format");
    if (!decodeAndDisplayPNG(imageData, imageSize)) {
      Serial.println("PNG decode failed");
    }
  } else {
    Serial.println("Unknown image format");
    clearDisplay();
  }

  Serial.println("Display update complete");
}

// =============================================================================
// STATUS DISPLAY
// =============================================================================

void displayProgressBar(int x, int y, int width, int height, int percent) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  display.drawRect(x, y, width, height, GxEPD_BLACK);

  int fillWidth = (width - 4) * percent / 100;
  if (fillWidth > 0) {
    display.fillRect(x + 2, y + 2, fillWidth, height - 4, GxEPD_BLACK);
  }
}

void displayStatus(const char *status, int progressPercent) {
  display.setRotation(0);
  display.setFullWindow();

  display.firstPage();
  do {
    display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);

    // Header bar
    display.fillRect(0, 0, DISPLAY_WIDTH, 60, GxEPD_BLUE);
    display.setTextColor(GxEPD_WHITE);
    display.setTextSize(2);
    display.setCursor(20, 20);
    display.print(DEVICE_NAME);

    // Status text
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(20, 100);
    display.print(status);

    // Progress bar
    if (progressPercent >= 0) {
      int barX = 50;
      int barY = 200;
      int barWidth = DISPLAY_WIDTH - 100;
      int barHeight = 30;

      displayProgressBar(barX, barY, barWidth, barHeight, progressPercent);

      display.setTextSize(2);
      display.setCursor(barX + barWidth / 2 - 30, barY + barHeight + 10);
      display.printf("%d%%", progressPercent);
    }

    // Version footer
    display.setTextSize(1);
    display.setCursor(20, DISPLAY_HEIGHT - 30);
    display.printf("Version: %s", FIRMWARE_VERSION);

  } while (display.nextPage());
}

// =============================================================================
// POWER MANAGEMENT
// =============================================================================

void displaySleep() {
  SPI.end();
  display.hibernate();

  // EPD CS (GPIO 26) is RTC-capable - hold HIGH to keep display deselected
  rtc_gpio_init((gpio_num_t)EPD_CS_PIN);
  rtc_gpio_set_direction((gpio_num_t)EPD_CS_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)EPD_CS_PIN, 1);
  rtc_gpio_hold_en((gpio_num_t)EPD_CS_PIN);

  // EPD DC (GPIO 25) IS RTC-capable (RTC_GPIO6) - hold LOW to prevent floating
  rtc_gpio_init((gpio_num_t)EPD_DC_PIN);
  rtc_gpio_set_direction((gpio_num_t)EPD_DC_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)EPD_DC_PIN, 0);
  rtc_gpio_hold_en((gpio_num_t)EPD_DC_PIN);

  // EPD RST (GPIO 14) - hold HIGH to avoid reset during sleep
  rtc_gpio_init((gpio_num_t)EPD_RST_PIN);
  rtc_gpio_set_direction((gpio_num_t)EPD_RST_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)EPD_RST_PIN, 1);
  rtc_gpio_hold_en((gpio_num_t)EPD_RST_PIN);

  // EPD BUSY (GPIO 4) is RTC-capable - NO pulls to avoid fighting external pullup
  // Display typically has 10k external pullup on BUSY; enabling internal pulldown
  // creates a resistor divider that wastes ~60-100µA continuously during sleep
  rtc_gpio_init((gpio_num_t)EPD_BUSY_PIN);
  rtc_gpio_set_direction((gpio_num_t)EPD_BUSY_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis((gpio_num_t)EPD_BUSY_PIN);
  rtc_gpio_pullup_dis((gpio_num_t)EPD_BUSY_PIN);
  rtc_gpio_hold_en((gpio_num_t)EPD_BUSY_PIN);

  // SPI pins (non-RTC GPIOs 18, 23, 19) - pulldown to prevent floating
  // Non-RTC pin state is lost during deep sleep, but IO MUX pulls persist on ESP32
  pinMode(SPI_SCK_PIN, INPUT_PULLDOWN);
  pinMode(SPI_MOSI_PIN, INPUT_PULLDOWN);
  pinMode(SPI_MISO_PIN, INPUT_PULLDOWN);

  Serial.println("Display in hibernate mode (pins configured for low power)");
}

void displayWake() {
  // Release RTC GPIO holds from sleep
  rtc_gpio_hold_dis((gpio_num_t)EPD_BUSY_PIN);
  rtc_gpio_deinit((gpio_num_t)EPD_BUSY_PIN);
  rtc_gpio_hold_dis((gpio_num_t)EPD_DC_PIN);
  rtc_gpio_deinit((gpio_num_t)EPD_DC_PIN);
  rtc_gpio_hold_dis((gpio_num_t)EPD_CS_PIN);
  rtc_gpio_deinit((gpio_num_t)EPD_CS_PIN);
  rtc_gpio_hold_dis((gpio_num_t)EPD_RST_PIN);
  rtc_gpio_deinit((gpio_num_t)EPD_RST_PIN);
}

void clearDisplay() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);
  } while (display.nextPage());
}
