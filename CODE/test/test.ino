#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_VEML7700.h>

// =====================================================
// TFT PINS - Arduino Mega 2560
// =====================================================
//
// Hardware SPI pins on Mega:
// MOSI = 51
// MISO = 50
// SCK  = 52
//
// Additional TFT control pins:
#define TFT_CS   10
#define TFT_DC   9
#define TFT_RST  8

// Create TFT display object
Adafruit_ILI9341 tft = Adafruit_ILI9341(
  TFT_CS,
  TFT_DC,
  TFT_RST
);

// =====================================================
// VEML7700 LIGHT SENSOR
// =====================================================

Adafruit_VEML7700 veml = Adafruit_VEML7700();

// Store current light level
float lux = 0.0;

// =====================================================
// FUNCTION: Draw the basic screen layout
// =====================================================

void drawInterface() {

  // Clear the entire screen
  tft.fillScreen(ILI9341_BLACK);

  // -----------------------------
  // TITLE
  // -----------------------------

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);

  tft.setCursor(15, 15);
  tft.println("VEML7700 LIGHT SENSOR");

  // Divider line
  tft.drawLine(
    10,
    45,
    310,
    45,
    ILI9341_WHITE
  );

  // -----------------------------
  // LIGHT LEVEL LABEL
  // -----------------------------

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);

  tft.setCursor(20, 65);
  tft.println("Light Level:");

  // -----------------------------
  // STATUS LABEL
  // -----------------------------

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);

  tft.setCursor(20, 175);
  tft.println("Status:");
}

// =====================================================
// FUNCTION: Display the lux value
// =====================================================

void displayLux(float value) {

  // Clear previous lux reading
  tft.fillRect(
    15,
    95,
    300,
    60,
    ILI9341_BLACK
  );

  // Show number
  tft.setTextColor(ILI9341_GREEN);
  tft.setTextSize(4);

  tft.setCursor(20, 105);

  tft.print(value, 1);

  // Show unit
  tft.setTextSize(2);
  tft.print(" lux");
}

// =====================================================
// FUNCTION: Display light condition
// =====================================================

void displayStatus(float value) {

  // Clear previous status
  tft.fillRect(
    15,
    200,
    300,
    35,
    ILI9341_BLACK
  );

  tft.setTextSize(2);
  tft.setCursor(20, 205);

  // Determine light level category
  if (value < 10) {

    tft.setTextColor(ILI9341_BLUE);
    tft.print("VERY DARK");

  }

  else if (value < 100) {

    tft.setTextColor(ILI9341_CYAN);
    tft.print("DIM");

  }

  else if (value < 1000) {

    tft.setTextColor(ILI9341_GREEN);
    tft.print("NORMAL LIGHT");

  }

  else if (value < 10000) {

    tft.setTextColor(ILI9341_YELLOW);
    tft.print("BRIGHT");

  }

  else {

    tft.setTextColor(ILI9341_RED);
    tft.print("VERY BRIGHT");

  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  // Start Serial Monitor
  Serial.begin(9600);

  Serial.println();
  Serial.println("==============================");
  Serial.println("VEML7700 + TFT TEST");
  Serial.println("==============================");

  // ===================================================
  // START I2C
  // ===================================================

  Wire.begin();

  Serial.println("I2C initialized.");

  // ===================================================
  // START TFT
  // ===================================================

  Serial.println("Starting TFT display...");

  tft.begin();

  // Landscape mode
  // 0 = portrait
  // 1 = landscape
  // 2 = portrait flipped
  // 3 = landscape flipped
  tft.setRotation(1);

  // Test screen
  tft.fillScreen(ILI9341_BLACK);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);

  tft.setCursor(20, 30);
  tft.println("TFT STARTING...");

  delay(1000);

  // ===================================================
  // START VEML7700
  // ===================================================

  Serial.println("Checking VEML7700 sensor...");

  if (!veml.begin()) {

    Serial.println("ERROR: VEML7700 not detected.");

    // Show sensor error
    tft.fillScreen(ILI9341_BLACK);

    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(3);

    tft.setCursor(20, 60);
    tft.println("ERROR!");

    tft.setTextSize(2);

    tft.setCursor(20, 110);
    tft.println("VEML7700");

    tft.setCursor(20, 140);
    tft.println("NOT FOUND");

    // Stop here if sensor is missing
    while (true) {
      delay(1000);
    }
  }

  // ===================================================
  // SENSOR CONFIGURATION
  // ===================================================

  Serial.println("VEML7700 detected.");

  // Default gain
  veml.setGain(VEML7700_GAIN_1);

  // Integration time
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("Sensor configured.");

  // ===================================================
  // DRAW TFT INTERFACE
  // ===================================================

  drawInterface();

  Serial.println();
  Serial.println("System ready.");
  Serial.println("==============================");
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  // ===================================================
  // READ LIGHT SENSOR
  // ===================================================

  lux = veml.readLux();

  // ===================================================
  // SHOW IN SERIAL MONITOR
  // ===================================================

  Serial.print("Light Level: ");
  Serial.print(lux, 2);
  Serial.println(" lux");

  // ===================================================
  // SHOW ON TFT
  // ===================================================

  displayLux(lux);

  displayStatus(lux);

  // Update twice per second
  delay(500);
}