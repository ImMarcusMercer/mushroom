#pragma once
#include <cstddef>
#include <cstdint>

// The Word document does not identify the TFT controller or its DC/RST pins.
// Keep your proven TFT_eSPI User_Setup.h. See docs/SETUP.md before uploading.
#ifndef MUSHROOM_ENABLE_TFT
#define MUSHROOM_ENABLE_TFT 1
#endif

namespace farm {
constexpr const char* VERSION = "1.1.0";
constexpr size_t SENSOR_COUNT = 3;
constexpr size_t SPECTRO_MAX = 6;
constexpr size_t HISTORY_CAPACITY = 800;
constexpr uint32_t SENSOR_TIMEOUT_MS = 30000;
constexpr uint32_t LOG_INTERVAL_MS = 3600000;
constexpr uint32_t LOG_RETRY_MS = 30000;
constexpr uint32_t SAMPLE_WINDOW_SECONDS = 300;
constexpr uint8_t SPECTRO_SETS_PER_WINDOW = 3;
constexpr float HUMIDITY_ON_BELOW = 75.0f;
constexpr float HUMIDITY_OFF_AT = 85.0f;
constexpr float EXHAUST_LOW = 22.0f;
constexpr float EXHAUST_HIGH = 29.0f;
// Preserve the original outside-range rule, including low-temperature exhaust.
constexpr bool EXHAUST_ON_LOW_TEMPERATURE = true;
constexpr uint32_t SPRINKLER_DURATION_MS = 30000;
constexpr uint8_t SPRINKLER_HOUR = 14;
constexpr uint8_t SPRINKLER_MINUTE = 0;
constexpr uint8_t LIGHTS_ON_HOUR = 16;
constexpr uint8_t LIGHTS_OFF_HOUR = 7;
constexpr uint32_t C3_POWER_OFF_MS = 5000;
constexpr uint32_t C3_RECOVERY_COOLDOWN_MS = 60000;
constexpr uint32_t C3_BOOT_GRACE_MS = 60000;
constexpr bool ENABLE_C3_POWER_RECOVERY = true;
constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;
constexpr uint8_t MCP_ADDRESS = 0x20;
constexpr int SD_SCK = 14;
constexpr int SD_MISO = 12;
constexpr int SD_MOSI = 13;
constexpr int SD_CS = 15;
constexpr int BUTTON_PIN = 33;
constexpr uint8_t RELAY_PINS[] = {0, 1, 2, 3, 4, 5};
constexpr uint8_t C3_POWER_PIN = 7;
constexpr size_t MAX_JSON_BYTES = 768;
static_assert(HUMIDITY_ON_BELOW < HUMIDITY_OFF_AT, "Humidity thresholds must be ordered");
static_assert(SPRINKLER_DURATION_MS > 0, "Pump duration must be positive");
static_assert(HISTORY_CAPACITY >= 32, "History capacity is too small");
}
