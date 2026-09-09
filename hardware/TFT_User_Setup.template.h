#pragma once
// TEMPLATE ONLY. Complete these settings for the actual display before copying
// them into TFT_eSPI's selected User_Setup.h. The Word source omits driver/DC/RST.
#define USER_SETUP_INFO "MushroomFarm classic ESP32 VSPI display"

// Uncomment exactly ONE matching driver, or use the correct TFT_eSPI driver
// definition for your hardware. These examples do not identify your display.
// #define ILI9488_DRIVER
// #define ST7796_DRIVER
// #define ILI9486_DRIVER
#if !defined(ILI9488_DRIVER) && !defined(ST7796_DRIVER) && !defined(ILI9486_DRIVER)
#error "Select the actual display driver in your TFT_eSPI setup."
#endif

#define TFT_MOSI 23
#define TFT_MISO 19
#define TFT_SCLK 18
#define TFT_CS 5
#define TFT_DC -1 // REQUIRED: replace with the actual DC GPIO.
#define TFT_RST -1 // Use actual RST GPIO, or -1 only if reset is wired appropriately.
#if TFT_DC < 0
#error "Set TFT_DC to the actual DC GPIO. It was not provided in the Word document."
#endif

#define LOAD_GLCD
#define SPI_FREQUENCY 20000000
#define SPI_READ_FREQUENCY 10000000
// Leave USE_HSPI_PORT undefined. HSPI is dedicated to the SD card.
