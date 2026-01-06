#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <GxEPD2_7C.h>
#include <SPI.h>
#include "config.h"

// Spectra 6 (7-color) e-paper display color definitions
#ifndef GxEPD_ORANGE
#define GxEPD_ORANGE 0xFD20
#endif

// Display type: Good Display 7.3" Spectra 6 (800x480)
using DisplayType = GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT / 4>;

// Global display object (defined in display.cpp)
extern DisplayType display;

// =============================================================================
// DISPLAY INITIALIZATION
// =============================================================================

// Initialize SPI and display driver
// Returns true on success
bool initDisplay();

// Reset display initialization flag (call before deep sleep)
void resetDisplayInit();

// =============================================================================
// IMAGE DISPLAY FUNCTIONS
// =============================================================================

// Display PNG image data on e-paper (requires full image in RAM)
// imageData: pointer to PNG file data
// imageSize: size of PNG data in bytes
void displayImageOnEPaper(uint8_t* imageData, size_t imageSize);

// Display PNG directly from file (memory efficient - streams from flash)
// filePath: path to PNG file in LittleFS
// Returns true on success
bool displayPNGFromFile(const char* filePath);

// =============================================================================
// STATUS DISPLAY FUNCTIONS
// =============================================================================

// Display status message with optional progress bar
void displayStatus(const char* status, int progressPercent = -1);

// Display progress bar at specified position
void displayProgressBar(int x, int y, int width, int height, int percent);

// =============================================================================
// POWER MANAGEMENT
// =============================================================================

// Put display into hibernate mode (lowest power)
void displaySleep();

// Wake display from hibernate
void displayWake();

// Clear display to white
void clearDisplay();

#endif // DISPLAY_H
