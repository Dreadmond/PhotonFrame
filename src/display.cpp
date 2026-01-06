#include "display.h"
#include "config.h"
#include "pngle.h"
#include <Adafruit_GFX.h>

// =============================================================================
// DISPLAY OBJECT
// =============================================================================
DisplayType display(GxEPD2_730c_GDEP073E01(
    /*CS=*/EPD_CS_PIN,
    /*DC=*/EPD_DC_PIN,
    /*RST=*/EPD_RST_PIN,
    /*BUSY=*/EPD_BUSY_PIN
));

// =============================================================================
// MODULE STATE
// =============================================================================
static bool displayInitialized = false;

// PNG decoder state
static uint32_t pngWidth = 0;
static uint32_t pngHeight = 0;
static float pngScale = 1.0f;
static int pngOffsetX = 0;
static int pngOffsetY = 0;
static bool pngDrawing = false;
static uint8_t* pngDataPtr = NULL;
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
        {0, 0, 0, GxEPD_BLACK},
        {255, 255, 255, GxEPD_WHITE},
        {200, 0, 0, GxEPD_RED},
        {255, 255, 0, GxEPD_YELLOW},
        {255, 128, 0, GxEPD_ORANGE},
        {0, 180, 0, GxEPD_GREEN},
        {0, 0, 200, GxEPD_BLUE},
    };

    uint32_t bestDistance = UINT32_MAX;
    uint16_t bestColor = GxEPD_WHITE;

    for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        const PaletteEntry& entry = palette[i];

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
// PNG DECODER CALLBACKS
// =============================================================================

void onPngInit(pngle_t* pngle, uint32_t w, uint32_t h) {
    pngWidth = w;
    pngHeight = h;

    // Calculate scaling to fit display
    float scaleX = (float)DISPLAY_WIDTH / (float)w;
    float scaleY = (float)DISPLAY_HEIGHT / (float)h;
    pngScale = (scaleX < scaleY) ? scaleX : scaleY;

    int drawWidth = (int)(w * pngScale);
    int drawHeight = (int)(h * pngScale);
    pngOffsetX = (DISPLAY_WIDTH - drawWidth) / 2;
    pngOffsetY = (DISPLAY_HEIGHT - drawHeight) / 2;
}

void onPngDraw(pngle_t* pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               const uint8_t rgba[4]) {
    if (!pngDrawing || x >= pngWidth || y >= pngHeight) return;

    uint16_t color = rgbTo7Color(rgba[0], rgba[1], rgba[2]);

    int drawX = pngOffsetX + (int)(x * pngScale);
    int drawY = pngOffsetY + (int)(y * pngScale);

    if (drawX >= 0 && drawX < DISPLAY_WIDTH &&
        drawY >= 0 && drawY < DISPLAY_HEIGHT) {
        display.drawPixel(drawX, drawY, color);
    }
}

void onPngDone(pngle_t* pngle) {
    // Nothing needed
}

// =============================================================================
// PNG DECODING
// =============================================================================

bool decodeAndDisplayPNG(uint8_t* pngData, size_t pngSize) {
    Serial.println("Decoding PNG image...");

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

    // Paging loop - decode PNG on every page pass
    display.firstPage();
    do {
        display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);

        pngle_t* pngle = pngle_new();
        if (pngle != NULL) {
            pngle_set_init_callback(pngle, onPngInit);
            pngle_set_draw_callback(pngle, onPngDraw);
            pngle_set_done_callback(pngle, onPngDone);

            pngDrawing = true;
            size_t fed = pngle_feed(pngle, pngDataPtr, pngDataSize);
            pngDrawing = false;

            pngle_destroy(pngle);

            if (fed != pngDataSize) {
                Serial.printf("WARNING: PNG feed incomplete: %d/%d\n",
                              fed, pngDataSize);
            }
        }
    } while (display.nextPage());

    pngDataPtr = NULL;
    pngDataSize = 0;

    if (pngWidth > 0 && pngHeight > 0) {
        Serial.printf("PNG rendered: %dx%d\n", pngWidth, pngHeight);
        return true;
    }

    Serial.println("ERROR: PNG decode failed");
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

    Serial.println("Initializing SPI...");
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, EPD_CS_PIN);
    delay(100);

    Serial.println("Initializing display driver...");
    display.init(115200, true);  // true = reset display
    delay(100);

    display.setRotation(0);

    displayInitialized = true;
    Serial.println("Display initialized");
    return true;
}

void resetDisplayInit() {
    displayInitialized = false;
}

// =============================================================================
// IMAGE DISPLAY
// =============================================================================

void displayImageOnEPaper(uint8_t* imageData, size_t imageSize) {
    Serial.printf("Displaying image: %d bytes\n", imageSize);

    if (imageData == NULL || imageSize == 0) {
        Serial.println("ERROR: Invalid image data");
        clearDisplay();
        return;
    }

    // Check for PNG format
    bool isPNG = (imageSize >= 8 &&
                  imageData[0] == 0x89 && imageData[1] == 0x50 &&
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
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    display.drawRect(x, y, width, height, GxEPD_BLACK);

    int fillWidth = (width - 4) * percent / 100;
    if (fillWidth > 0) {
        display.fillRect(x + 2, y + 2, fillWidth, height - 4, GxEPD_BLACK);
    }
}

void displayStatus(const char* status, int progressPercent) {
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
    display.hibernate();
    SPI.end();
    Serial.println("Display in hibernate mode");
}

void displayWake() {
    // GxEPD2 handles wake automatically during operations
}

void clearDisplay() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GxEPD_WHITE);
    } while (display.nextPage());
}
