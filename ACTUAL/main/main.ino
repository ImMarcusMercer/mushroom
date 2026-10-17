/*
 * MushroomFarm Monitor - Reconstructed Main Controller
 * Target: ZY-ESP32 / classic ESP32 (ESP32-WROOM-32 class)
 *
 * This sketch recreates the flow of the legacy Mushroom Code document and
 * adds a minimal multi-sensor payload path for the current node test:
 *   - 3 x VEML7700 lux sensors per node (current test)
 *   - 1 x DHT11 temperature/humidity sensor per node (current test)
 *   - moisture support retained in code but disabled for this test
 *   - data model remains sized for 9 lux + 3 moisture sensors per node
 *
 * Legacy behavior retained:
 *   - Wi-Fi AP + HTTP POST /data
 *   - TFT display
 *   - DS3231 RTC
 *   - SD logging on HSPI
 *   - MCP23017 relay control
 *   - legacy temperature/humidity + spectrometer handling
 *   - stale-node detection and C3 power-cycle relay
 *   - local web interface and serial maintenance commands
 *
 * Current test behavior added:
 *   - accepts JSON fields: temp, hum, lux[] and moisture_raw[]
 *   - third TFT page shows node DHT11 + lux data
 *   - moisture parsing/storage remains available but nodes currently send []
 *   - optional node-sensor CSV logging
 *
 * Main pin map preserved from the legacy document:
 *   TFT VSPI: MOSI=23, MISO=19, SCK=18, CS=5
 *   SD  HSPI: MOSI=13, MISO=12, SCK=14, CS=15
 *   I2C: SDA=21, SCL=22
 *   Button: GPIO33
 *
 * NOTE: TFT_eSPI still requires the correct display driver/DC/RST settings in
 * its User_Setup configuration. Those values were not present in the legacy
 * source document, so this sketch intentionally does not invent them.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <string.h>
#include "RTClib.h"
#include "SD.h"
#include "Adafruit_MCP23X17.h"

// -----------------------------------------------------------------------------
// Hardware objects
// -----------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
SPIClass hspi(HSPI);
RTC_DS3231 rtc;
Adafruit_MCP23X17 mcp;
WebServer server(80);

// -----------------------------------------------------------------------------
// Main hardware status
// -----------------------------------------------------------------------------
bool rtcAvailable = false;
bool sdAvailable = false;
bool mcpAvailable = false;

// -----------------------------------------------------------------------------
// Legacy pin assignments and relay mapping
// -----------------------------------------------------------------------------
#define SD_CS_PIN 15
#define BUTTON_PIN 33

#define RELAY1 0
#define RELAY2 1
#define RELAY3 2
#define EXHAUST_FAN 3
#define SPRINKLER_PUMP 4
#define LIGHTS 5
#define RELAY_C3_POWER 7

// -----------------------------------------------------------------------------
// Legacy automation values preserved from executable behavior
// -----------------------------------------------------------------------------
const float HUMIDITY_LOW = 75.0f;
const float HUMIDITY_HIGH = 85.0f;
const float TEMP_EXHAUST_HIGH = 29.0f;
const float TEMP_EXHAUST_LOW = 22.0f;

// The legacy file's constant is 30000 ms even though an old comment said 10 s.
const unsigned long SPRINKLER_DURATION = 30000UL;
const int RTC_DELAY_SECONDS = 2;
const long MANILA_OFFSET_HOURS = 8;

// -----------------------------------------------------------------------------
// Relay state and manual-override state
// -----------------------------------------------------------------------------
bool mistMaker1State = false;
bool mistMaker2State = false;
bool mistMaker3State = false;
bool exhaustFanState = false;
bool sprinklerPumpState = false;
bool lightsState = false;

bool mistMaker1ManualOverride = false;
bool mistMaker2ManualOverride = false;
bool mistMaker3ManualOverride = false;
bool exhaustFanManualOverride = false;
bool sprinklerPumpManualOverride = false;
bool lightsManualOverride = false;

unsigned long sprinklerStartTime = 0;
bool sprinklerActiveThisSession = false;

// -----------------------------------------------------------------------------
// Logging timers
// -----------------------------------------------------------------------------
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 3600000UL;  // 1 hour

unsigned long lastTimeUpdate = 0;
const unsigned long TIME_UPDATE_INTERVAL = 1000UL;

// -----------------------------------------------------------------------------
// Display/button state
// -----------------------------------------------------------------------------
bool lastButtonState = HIGH;
bool currentButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50UL;

// Legacy had two pages. A third page is added for the node multi-sensor test.
enum DisplayMode : uint8_t {
  MODE_TEMP_HUM = 0,
  MODE_SPECTRO = 1,
  MODE_NODE_SENSORS = 2
};

DisplayMode displayMode = MODE_TEMP_HUM;
DisplayMode currentDisplayMode = MODE_TEMP_HUM;
bool displayNeedsUpdate = false;
bool displayInitialized = false;
bool hasReceivedData = false;

// -----------------------------------------------------------------------------
// Wi-Fi AP / HTTP server
// -----------------------------------------------------------------------------
const char* ssid = "MushrooMan";
const char* password = "RABE2026qt";

// -----------------------------------------------------------------------------
// Sensor state
// -----------------------------------------------------------------------------
const int MAX_SENSORS = 10;
const int MAX_LUX_PER_NODE = 9;
const int MAX_DHT_PER_NODE = 3;
const int MAX_MOISTURE_PER_NODE = 3;

const unsigned long SENSOR_TIMEOUT_MS = 30000UL;

// Shared C3 power-cycle relay behavior preserved.
bool relayCycleActive = false;
unsigned long relayCycleStart = 0;
unsigned long lastRelayCycleMillis = 0;
const unsigned long RELAY_OFF_DURATION = 5000UL;
const unsigned long RELAY_CYCLE_COOLDOWN_MS = 60000UL;

// Current bench test: only Node 1 is required to be online.
// Change EXPECTED_C3_COUNT to 3 after Node 2 and Node 3 are powered and tested.
const char* expectedC3Ids[] = {"sensor1", "sensor2", "sensor3"};
const int EXPECTED_C3_COUNT = 1;

struct SensorData {
  String id;

  float temp = 0.0f;
  float hum = 0.0f;
  bool hasTemp = false;
  bool hasHum = false;

  float dhtTemp[MAX_DHT_PER_NODE] = {0};
  float dhtHum[MAX_DHT_PER_NODE] = {0};
  bool dhtValid[MAX_DHT_PER_NODE] = {false};
  uint8_t dhtCount = 0;

  String lastUpdated;
  unsigned long lastUpdateMillis = 0;

  int spectro[6] = {0};
  int spectroCount = 0;

  float lux[MAX_LUX_PER_NODE] = {0};
  bool luxValid[MAX_LUX_PER_NODE] = {false};
  uint8_t luxCount = 0;

  int moistureRaw[MAX_MOISTURE_PER_NODE] = {0};
  bool moistureValid[MAX_MOISTURE_PER_NODE] = {false};
  uint8_t moistureCount = 0;

  uint32_t sequence = 0;
  bool hasSequence = false;

  bool dataChanged = false;
};

SensorData sensors[MAX_SENSORS];
int sensorCount = 0;

// -----------------------------------------------------------------------------
// Legacy historical buffer: temperature/humidity + spectro
// -----------------------------------------------------------------------------
const int MAX_HISTORY_ENTRIES = 300;  // RAM-safe for classic ESP32; >1 hour at legacy worst-case rate

struct HistoricalReading {
  char sensorId[12];
  char treatmentId[12];
  float temp;
  float hum;
  char dateTime[12];
  char timeOnly[12];
  int spectro[3];
  int spectroCount;
  int readingIndex;
  unsigned long timestamp;
  unsigned long windowId;
  bool isValid;
};

HistoricalReading dataHistory[MAX_HISTORY_ENTRIES];
int historyCount = 0;

// -----------------------------------------------------------------------------
// New historical buffer: lux + moisture. One latest row/node/5-minute window.
// -----------------------------------------------------------------------------
const int MAX_NODE_HISTORY_ENTRIES = 72;  // 3 nodes x 12 five-minute windows/hour x 2 hours

struct NodeSensorHistory {
  char sensorId[12];
  float dhtTemp[MAX_DHT_PER_NODE];
  float dhtHum[MAX_DHT_PER_NODE];
  bool dhtValid[MAX_DHT_PER_NODE];
  uint8_t dhtCount;
  float lux[MAX_LUX_PER_NODE];
  bool luxValid[MAX_LUX_PER_NODE];
  uint8_t luxCount;
  int moistureRaw[MAX_MOISTURE_PER_NODE];
  bool moistureValid[MAX_MOISTURE_PER_NODE];
  uint8_t moistureCount;
  uint32_t sequence;
  char dateTime[12];
  char timeOnly[12];
  unsigned long timestamp;
  unsigned long windowId;
  bool isValid;
};

NodeSensorHistory nodeHistory[MAX_NODE_HISTORY_ENTRIES];
int nodeHistoryCount = 0;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
unsigned long calculateDayOfYear(int year, int month, int day);
unsigned long getCurrentWindowId();
int findSensorIndex(const String& id);
bool isSensorStale(int sensorIndex);
void clearStaleSensors();
String getDateTime();
String getTimeStamp();
String getDateOnly();
String getTimeOnly();
float getRTCTemperature();
void setRTCTime(int year, int month, int day, int hour, int minute, int second);
void getSortedSensorIndices(int sortedIndices[], int& count);

bool initializeSDCard();
bool testSDCardWrite();
void getSDCardInfo();
void listSDCardFiles();
String generateLogFilename(bool isTempHumid);
String generateNodeLogFilename();
bool createSDFile(String filename, String content);
String getMistmakerStatus(String sensorId);

void syncLegacyTempHumFromDHT(SensorData& sensor) {
  float tempSum = 0.0f;
  float humSum = 0.0f;
  uint8_t validCount = 0;
  for (int i = 0; i < sensor.dhtCount && i < MAX_DHT_PER_NODE; i++) {
    if (sensor.dhtValid[i]) {
      tempSum += sensor.dhtTemp[i];
      humSum += sensor.dhtHum[i];
      validCount++;
    }
  }
  if (validCount > 0) {
    sensor.temp = tempSum / validCount;
    sensor.hum = humSum / validCount;
    sensor.hasTemp = true;
    sensor.hasHum = true;
  }
}

void addToDataHistory(String sensorId, float temp, float hum, int* spectro, int spectroCount);
void addNodeSensorHistory(const SensorData& sensor);
void clearDataHistory();
void clearNodeHistory();
void logTempHumidityData();
void logSpectroData();
void logNodeSensorData();
void logSensorDataToCSV();

void handleData();
void handleRoot();
void handleTimeSync();
void handleRelayControl();
void handleResetOverrides();

void drawSystemStatus();
void drawTable();
void displayTempHumidity();
void displaySpectroReadings();
void displayNodeSensors();
void displayTreatmentColumn(int columnIndex, int colWidth, int startY, int treatmentNum,
                            const char* sensorId, int startIdx, int endIdx,
                            uint16_t textColor = TFT_GREEN);
void updateSensorRow(int sensorIndex, int rowY);
void updateTreatmentData();
void clearTreatmentColumn(int columnIndex, int colWidth, int startY);
void updateSystemStatus();
void handleButton();

void onMistMakerOne();
void offMistMakerOne();
void onMistMakerTwo();
void offMistMakerTwo();
void onMistMakerThree();
void offMistMakerThree();
void onExhaustFan();
void offExhaustFan();
void onSprinklerPump();
void offSprinklerPump();
void onLights();
void offLights();
bool isSprinklerTime();
bool isLightsTime();
bool areLightsOn();
void controlMistMakers();
void controlExhaustFan();
void controlSprinklerPump();
void controlLights();
void controlAllRelays();
void monitorC3AndCycleRelay();

// -----------------------------------------------------------------------------
// Time helpers
// -----------------------------------------------------------------------------
unsigned long calculateDayOfYear(int year, int month, int day) {
  int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool isLeapYear = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  if (isLeapYear) {
    daysInMonth[1] = 29;
  }

  unsigned long dayOfYear = day;
  for (int i = 0; i < month - 1; i++) {
    dayOfYear += daysInMonth[i];
  }
  return dayOfYear;
}

unsigned long getCurrentWindowId() {
  if (!rtcAvailable) {
    return millis() / 300000UL;
  }

  DateTime now = rtc.now();
  unsigned long windowInHour = now.minute() / 5;
  unsigned long dayOfYear = calculateDayOfYear(now.year(), now.month(), now.day());
  return (dayOfYear * 288UL) + (now.hour() * 12UL) + windowInHour;
}

String getDateTime() {
  if (!rtcAvailable) {
    return "RTC Not Available";
  }

  DateTime now = rtc.now();
  int hour12 = now.hour();
  String ampm = "AM";

  if (hour12 == 0) {
    hour12 = 12;
  } else if (hour12 > 12) {
    hour12 -= 12;
    ampm = "PM";
  } else if (hour12 == 12) {
    ampm = "PM";
  }

  char dateTime[28];
  snprintf(dateTime, sizeof(dateTime), "%02d/%02d/%04d %02d:%02d:%02d %s",
           now.day(), now.month(), now.year(), hour12, now.minute(), now.second(),
           ampm.c_str());
  return String(dateTime);
}

String getTimeStamp() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    int hour12 = now.hour();
    String ampm = "AM";

    if (hour12 == 0) {
      hour12 = 12;
    } else if (hour12 > 12) {
      hour12 -= 12;
      ampm = "PM";
    } else if (hour12 == 12) {
      ampm = "PM";
    }

    char timeStamp[16];
    snprintf(timeStamp, sizeof(timeStamp), "%02d:%02d:%02d%s",
             hour12, now.minute(), now.second(), ampm.c_str());
    return String(timeStamp);
  }

  unsigned long totalSeconds = millis() / 1000UL;
  unsigned long hours = totalSeconds / 3600UL;
  unsigned long minutes = (totalSeconds % 3600UL) / 60UL;
  unsigned long seconds = totalSeconds % 60UL;
  char timeStamp[16];
  snprintf(timeStamp, sizeof(timeStamp), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(timeStamp);
}

String getDateOnly() {
  if (!rtcAvailable) {
    return "No_Date";
  }
  DateTime now = rtc.now();
  char dateOnly[12];
  snprintf(dateOnly, sizeof(dateOnly), "%02d/%02d/%04d", now.day(), now.month(), now.year());
  return String(dateOnly);
}

String getTimeOnly() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    int hour12 = now.hour();
    String ampm = "AM";

    if (hour12 == 0) {
      hour12 = 12;
    } else if (hour12 > 12) {
      hour12 -= 12;
      ampm = "PM";
    } else if (hour12 == 12) {
      ampm = "PM";
    }

    char timeOnly[16];
    snprintf(timeOnly, sizeof(timeOnly), "%02d:%02d:%02d %s",
             hour12, now.minute(), now.second(), ampm.c_str());
    return String(timeOnly);
  }

  unsigned long totalSeconds = millis() / 1000UL;
  unsigned long hours = totalSeconds / 3600UL;
  unsigned long minutes = (totalSeconds % 3600UL) / 60UL;
  unsigned long seconds = totalSeconds % 60UL;
  char timeOnly[16];
  snprintf(timeOnly, sizeof(timeOnly), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(timeOnly);
}

float getRTCTemperature() {
  if (!rtcAvailable) {
    return 0.0f;
  }
  return rtc.getTemperature();
}

void setRTCTime(int year, int month, int day, int hour, int minute, int second) {
  if (!rtcAvailable) {
    Serial.println("RTC not available - cannot set time");
    return;
  }
  rtc.adjust(DateTime(year, month, day, hour, minute, second));
  Serial.println("RTC time updated to: " + getDateTime());
}

// -----------------------------------------------------------------------------
// Sensor helpers
// -----------------------------------------------------------------------------
void getSortedSensorIndices(int sortedIndices[], int& count) {
  count = sensorCount;
  for (int i = 0; i < sensorCount; i++) {
    sortedIndices[i] = i;
  }

  for (int i = 0; i < sensorCount - 1; i++) {
    for (int j = 0; j < sensorCount - i - 1; j++) {
      if (sensors[sortedIndices[j]].id.compareTo(sensors[sortedIndices[j + 1]].id) > 0) {
        int temp = sortedIndices[j];
        sortedIndices[j] = sortedIndices[j + 1];
        sortedIndices[j + 1] = temp;
      }
    }
  }
}

int findSensorIndex(const String& id) {
  for (int i = 0; i < sensorCount; i++) {
    if (sensors[i].id == id) {
      return i;
    }
  }
  return -1;
}

bool isSensorStale(int sensorIndex) {
  if (sensorIndex < 0 || sensorIndex >= sensorCount) {
    return true;
  }
  if (sensors[sensorIndex].lastUpdateMillis == 0) {
    return true;
  }
  return (millis() - sensors[sensorIndex].lastUpdateMillis) > SENSOR_TIMEOUT_MS;
}

void clearStaleSensors() {
  for (int i = 0; i < sensorCount; i++) {
    if (!isSensorStale(i)) {
      continue;
    }

    sensors[i].temp = 0.0f;
    sensors[i].hum = 0.0f;
    sensors[i].hasTemp = false;
    sensors[i].hasHum = false;
    sensors[i].spectroCount = 0;
    sensors[i].dhtCount = 0;
    sensors[i].luxCount = 0;
    sensors[i].moistureCount = 0;

    for (int j = 0; j < MAX_DHT_PER_NODE; j++) {
      sensors[i].dhtTemp[j] = 0.0f;
      sensors[i].dhtHum[j] = 0.0f;
      sensors[i].dhtValid[j] = false;
    }
    for (int j = 0; j < MAX_LUX_PER_NODE; j++) {
      sensors[i].lux[j] = 0.0f;
      sensors[i].luxValid[j] = false;
    }
    for (int j = 0; j < MAX_MOISTURE_PER_NODE; j++) {
      sensors[i].moistureRaw[j] = 0;
      sensors[i].moistureValid[j] = false;
    }

    sensors[i].dataChanged = true;
    displayNeedsUpdate = true;
    Serial.println("Cleared stale sensor: " + sensors[i].id);
  }
}

// -----------------------------------------------------------------------------
// Legacy historical data management
// -----------------------------------------------------------------------------
void addToDataHistory(String sensorId, float temp, float hum, int* spectro, int spectroCount) {
  unsigned long currentWindow = getCurrentWindowId();
  char sensorIdBuf[16];
  sensorId.toCharArray(sensorIdBuf, sizeof(sensorIdBuf));

  // Keep the latest temp/humidity value per sensor per five-minute window.
  int existingIndex = -1;
  for (int i = 0; i < historyCount; i++) {
    if (dataHistory[i].isValid &&
        strcmp(dataHistory[i].sensorId, sensorIdBuf) == 0 &&
        dataHistory[i].windowId == currentWindow &&
        strlen(dataHistory[i].treatmentId) == 0) {
      existingIndex = i;
      break;
    }
  }

  if (existingIndex >= 0) {
    HistoricalReading* entry = &dataHistory[existingIndex];
    entry->temp = temp;
    entry->hum = hum;
    String dateStr = getDateOnly();
    String timeStr = getTimeOnly();
    strncpy(entry->dateTime, dateStr.c_str(), sizeof(entry->dateTime) - 1);
    entry->dateTime[sizeof(entry->dateTime) - 1] = '\0';
    strncpy(entry->timeOnly, timeStr.c_str(), sizeof(entry->timeOnly) - 1);
    entry->timeOnly[sizeof(entry->timeOnly) - 1] = '\0';
    entry->timestamp = millis();
  } else {
    if (historyCount >= MAX_HISTORY_ENTRIES) {
      int removeCount = MAX_HISTORY_ENTRIES / 5;
      for (int i = 0; i < historyCount - removeCount; i++) {
        dataHistory[i] = dataHistory[i + removeCount];
      }
      historyCount -= removeCount;
    }

    HistoricalReading* entry = &dataHistory[historyCount++];
    memset(entry, 0, sizeof(HistoricalReading));
    strncpy(entry->sensorId, sensorIdBuf, sizeof(entry->sensorId) - 1);
    entry->treatmentId[0] = '\0';
    entry->temp = temp;
    entry->hum = hum;
    String dateStr = getDateOnly();
    String timeStr = getTimeOnly();
    strncpy(entry->dateTime, dateStr.c_str(), sizeof(entry->dateTime) - 1);
    strncpy(entry->timeOnly, timeStr.c_str(), sizeof(entry->timeOnly) - 1);
    entry->spectroCount = 0;
    entry->timestamp = millis();
    entry->windowId = currentWindow;
    entry->isValid = true;
  }

  if (spectroCount <= 0) {
    return;
  }

  struct TreatmentData {
    const char* treatmentId;
    int startIdx;
  } treatments[2];

  int treatmentCount = 0;
  if (sensorId == "sensor1" && spectroCount >= 3) {
    treatments[treatmentCount++] = {"treatment1", 0};
    if (spectroCount >= 6) {
      treatments[treatmentCount++] = {"treatment2", 3};
    }
  } else if (sensorId == "sensor2" && spectroCount >= 3) {
    treatments[treatmentCount++] = {"treatment3", 0};
    if (spectroCount >= 6) {
      treatments[treatmentCount++] = {"treatment4", 3};
    }
  } else if (sensorId == "sensor3" && spectroCount >= 3) {
    treatments[treatmentCount++] = {"treatment5", 0};
  }

  for (int t = 0; t < treatmentCount; t++) {
    int existingCount = 0;
    for (int i = 0; i < historyCount; i++) {
      if (dataHistory[i].isValid &&
          strcmp(dataHistory[i].treatmentId, treatments[t].treatmentId) == 0 &&
          dataHistory[i].windowId == currentWindow) {
        existingCount++;
      }
    }

    if (existingCount >= 3) {
      Serial.println("Window already has 3 spectro sets for " + String(treatments[t].treatmentId));
      continue;
    }

    if (historyCount >= MAX_HISTORY_ENTRIES) {
      int removeCount = MAX_HISTORY_ENTRIES / 10;
      for (int i = 0; i < historyCount - removeCount; i++) {
        dataHistory[i] = dataHistory[i + removeCount];
      }
      historyCount -= removeCount;
    }

    HistoricalReading* entry = &dataHistory[historyCount++];
    memset(entry, 0, sizeof(HistoricalReading));
    strncpy(entry->sensorId, sensorIdBuf, sizeof(entry->sensorId) - 1);
    strncpy(entry->treatmentId, treatments[t].treatmentId, sizeof(entry->treatmentId) - 1);
    entry->temp = 0.0f;
    entry->hum = 0.0f;
    String dateStr = getDateOnly();
    String timeStr = getTimeOnly();
    strncpy(entry->dateTime, dateStr.c_str(), sizeof(entry->dateTime) - 1);
    strncpy(entry->timeOnly, timeStr.c_str(), sizeof(entry->timeOnly) - 1);
    entry->spectroCount = 3;
    entry->readingIndex = existingCount;
    entry->timestamp = millis();
    entry->windowId = currentWindow;
    entry->isValid = true;

    for (int i = 0; i < 3 && (treatments[t].startIdx + i) < spectroCount; i++) {
      entry->spectro[i] = spectro[treatments[t].startIdx + i];
    }
  }
}

void clearDataHistory() {
  historyCount = 0;
  for (int i = 0; i < MAX_HISTORY_ENTRIES; i++) {
    dataHistory[i].isValid = false;
  }
}

// -----------------------------------------------------------------------------
// Lux/moisture history management
// -----------------------------------------------------------------------------
void addNodeSensorHistory(const SensorData& sensor) {
  unsigned long currentWindow = getCurrentWindowId();
  int existingIndex = -1;

  for (int i = 0; i < nodeHistoryCount; i++) {
    if (nodeHistory[i].isValid &&
        sensor.id == String(nodeHistory[i].sensorId) &&
        nodeHistory[i].windowId == currentWindow) {
      existingIndex = i;
      break;
    }
  }

  if (existingIndex < 0) {
    if (nodeHistoryCount >= MAX_NODE_HISTORY_ENTRIES) {
      int removeCount = MAX_NODE_HISTORY_ENTRIES / 5;
      for (int i = 0; i < nodeHistoryCount - removeCount; i++) {
        nodeHistory[i] = nodeHistory[i + removeCount];
      }
      nodeHistoryCount -= removeCount;
    }
    existingIndex = nodeHistoryCount++;
  }

  NodeSensorHistory* entry = &nodeHistory[existingIndex];
  memset(entry, 0, sizeof(NodeSensorHistory));
  sensor.id.toCharArray(entry->sensorId, sizeof(entry->sensorId));
  entry->luxCount = sensor.luxCount;
  entry->moistureCount = sensor.moistureCount;
  entry->sequence = sensor.sequence;

  for (int i = 0; i < MAX_LUX_PER_NODE; i++) {
    entry->lux[i] = sensor.lux[i];
    entry->luxValid[i] = sensor.luxValid[i];
  }
  for (int i = 0; i < MAX_MOISTURE_PER_NODE; i++) {
    entry->moistureRaw[i] = sensor.moistureRaw[i];
    entry->moistureValid[i] = sensor.moistureValid[i];
  }

  String dateStr = getDateOnly();
  String timeStr = getTimeOnly();
  strncpy(entry->dateTime, dateStr.c_str(), sizeof(entry->dateTime) - 1);
  strncpy(entry->timeOnly, timeStr.c_str(), sizeof(entry->timeOnly) - 1);
  entry->timestamp = millis();
  entry->windowId = currentWindow;
  entry->isValid = true;
}

void clearNodeHistory() {
  nodeHistoryCount = 0;
  for (int i = 0; i < MAX_NODE_HISTORY_ENTRIES; i++) {
    nodeHistory[i].isValid = false;
  }
}

// -----------------------------------------------------------------------------
// SD support
// -----------------------------------------------------------------------------
bool testSDCardWrite() {
  File testFile = SD.open("/test.txt", FILE_WRITE);
  if (!testFile) {
    Serial.println("SD card write test failed");
    return false;
  }

  testFile.println("Test OK");
  testFile.close();
  SD.remove("/test.txt");
  Serial.println("SD card write test passed");
  return true;
}

void getSDCardInfo() {
  uint64_t totalBytes = SD.totalBytes() / (1024ULL * 1024ULL);
  uint64_t usedBytes = SD.usedBytes() / (1024ULL * 1024ULL);
  Serial.println("SD Card: " + String((unsigned long)totalBytes) + "MB, " +
                 String((unsigned long)usedBytes) + "MB used");
}

bool initializeSDCard() {
  Serial.println("Initializing SD Card on HSPI bus...");
  hspi.begin(14, 12, 13, 15);  // SCK, MISO, MOSI, CS

  if (!SD.begin(SD_CS_PIN, hspi)) {
    Serial.println("SD Card initialization failed");
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card detected");
    return false;
  }

  getSDCardInfo();
  if (!testSDCardWrite()) {
    return false;
  }

  listSDCardFiles();
  return true;
}

void listSDCardFiles() {
  File root = SD.open("/");
  if (!root) {
    return;
  }

  int fileCount = 0;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      fileCount++;
      Serial.println(String(file.name()) + " (" + String(file.size()) + " bytes)");
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  Serial.println("Total: " + String(fileCount) + " files");
}

String makeTimestampPrefix() {
  if (!rtcAvailable) {
    return "uptime" + String(millis() / 60000UL);
  }

  DateTime now = rtc.now();
  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char dateTime[24];
  snprintf(dateTime, sizeof(dateTime), "%02d%s%04d-%02d%02d",
           now.day(), months[now.month() - 1], now.year(), now.hour(), now.minute());
  return String(dateTime);
}

String generateLogFilename(bool isTempHumid) {
  return "/" + makeTimestampPrefix() + (isTempHumid ? "-temp.csv" : "-spectro.csv");
}

String generateNodeLogFilename() {
  return "/" + makeTimestampPrefix() + "-node-sensors.csv";
}

bool createSDFile(String filename, String content) {
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Cannot create file: " + filename);
    return false;
  }

  file.print(content);
  file.close();
  Serial.println("File written: " + filename);
  return true;
}

String getMistmakerStatus(String sensorId) {
  if (sensorId == "sensor1") return mistMaker1State ? "ON" : "OFF";
  if (sensorId == "sensor2") return mistMaker2State ? "ON" : "OFF";
  if (sensorId == "sensor3") return mistMaker3State ? "ON" : "OFF";
  return "N/A";
}

void logTempHumidityData() {
  if (historyCount == 0) {
    return;
  }

  String filename = generateLogFilename(true);
  String content = "Date,Time,Sensor,Temperature,Humidity,Status\n";
  int logCount = 0;

  for (int i = 0; i < historyCount; i++) {
    if (!dataHistory[i].isValid || strlen(dataHistory[i].treatmentId) != 0) {
      continue;
    }

    content += String(dataHistory[i].dateTime) + ",";
    content += String(dataHistory[i].timeOnly) + ",";
    content += String(dataHistory[i].sensorId) + ",";
    content += String(dataHistory[i].temp, 2) + ",";
    content += String(dataHistory[i].hum, 2) + ",";
    content += getMistmakerStatus(String(dataHistory[i].sensorId)) + "\n";
    logCount++;
  }

  if (logCount > 0) {
    createSDFile(filename, content);
  }
}

void logSpectroData() {
  if (historyCount == 0) {
    return;
  }

  String filename = generateLogFilename(false);
  String content = "Date,Time,Treatment1,Treatment2,Treatment3,Treatment4,Treatment5\n";

  unsigned long uniqueWindows[200];
  int uniqueWindowCount = 0;

  for (int i = 0; i < historyCount; i++) {
    if (!dataHistory[i].isValid || strlen(dataHistory[i].treatmentId) == 0) {
      continue;
    }

    bool found = false;
    for (int j = 0; j < uniqueWindowCount; j++) {
      if (uniqueWindows[j] == dataHistory[i].windowId) {
        found = true;
        break;
      }
    }

    if (!found && uniqueWindowCount < 200) {
      uniqueWindows[uniqueWindowCount++] = dataHistory[i].windowId;
    }
  }

  int logCount = 0;
  for (int w = 0; w < uniqueWindowCount; w++) {
    String dateTime = "";
    String timeOnly = "";
    String treatmentValues[5];
    bool hasData = false;

    for (int i = 0; i < historyCount; i++) {
      if (!dataHistory[i].isValid ||
          dataHistory[i].windowId != uniqueWindows[w] ||
          strlen(dataHistory[i].treatmentId) == 0 ||
          dataHistory[i].spectroCount < 3) {
        continue;
      }

      int treatmentIndex = -1;
      String treatmentId = String(dataHistory[i].treatmentId);
      if (treatmentId == "treatment1") treatmentIndex = 0;
      else if (treatmentId == "treatment2") treatmentIndex = 1;
      else if (treatmentId == "treatment3") treatmentIndex = 2;
      else if (treatmentId == "treatment4") treatmentIndex = 3;
      else if (treatmentId == "treatment5") treatmentIndex = 4;

      if (treatmentIndex < 0) {
        continue;
      }

      if (treatmentValues[treatmentIndex].length() > 0) {
        treatmentValues[treatmentIndex] += " ";
      }
      treatmentValues[treatmentIndex] += String(dataHistory[i].spectro[0]) + " " +
                                         String(dataHistory[i].spectro[1]) + " " +
                                         String(dataHistory[i].spectro[2]);

      if (!hasData) {
        dateTime = String(dataHistory[i].dateTime);
        timeOnly = String(dataHistory[i].timeOnly);
        hasData = true;
      }
    }

    if (!hasData) {
      continue;
    }

    content += dateTime + "," + timeOnly + ",";
    for (int t = 0; t < 5; t++) {
      content += "\"" + treatmentValues[t] + "\"";
      if (t < 4) content += ",";
    }
    content += "\n";
    logCount++;
  }

  if (logCount > 0) {
    createSDFile(filename, content);
  }
}

void logNodeSensorData() {
  if (nodeHistoryCount == 0) {
    return;
  }
  String filename = generateNodeLogFilename();
  String content = "Date,Time,Sensor,Sequence";
  for (int i = 0; i < MAX_DHT_PER_NODE; i++) {
    content += ",DHT" + String(i + 1) + "_TempC";
    content += ",DHT" + String(i + 1) + "_HumPct";
  }
  for (int i = 0; i < MAX_LUX_PER_NODE; i++) content += ",Lux" + String(i + 1);
  for (int i = 0; i < MAX_MOISTURE_PER_NODE; i++) content += ",MoistureRaw" + String(i + 1);
  content += "\n";
  for (int i = 0; i < nodeHistoryCount; i++) {
    if (!nodeHistory[i].isValid) continue;
    content += String(nodeHistory[i].dateTime) + "," + String(nodeHistory[i].timeOnly) + "," + String(nodeHistory[i].sensorId) + "," + String(nodeHistory[i].sequence);
    for (int j = 0; j < MAX_DHT_PER_NODE; j++) {
      content += ",";
      if (nodeHistory[i].dhtValid[j]) content += String(nodeHistory[i].dhtTemp[j], 1);
      content += ",";
      if (nodeHistory[i].dhtValid[j]) content += String(nodeHistory[i].dhtHum[j], 1);
    }
    for (int j = 0; j < MAX_LUX_PER_NODE; j++) {
      content += ",";
      if (nodeHistory[i].luxValid[j]) content += String(nodeHistory[i].lux[j], 2);
    }
    for (int j = 0; j < MAX_MOISTURE_PER_NODE; j++) {
      content += ",";
      if (nodeHistory[i].moistureValid[j]) content += String(nodeHistory[i].moistureRaw[j]);
    }
    content += "\n";
  }
  createSDFile(filename, content);
}

void logSensorDataToCSV() {
  if (historyCount == 0 && nodeHistoryCount == 0) {
    Serial.println("No historical data to log");
    return;
  }

  Serial.println("Starting data logging session...");
  logTempHumidityData();
  delay(100);
  logSpectroData();
  delay(100);
  logNodeSensorData();

  clearDataHistory();
  clearNodeHistory();
  Serial.println("Data logging session completed");
}

// -----------------------------------------------------------------------------
// HTTP data input
// -----------------------------------------------------------------------------
void handleData() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String body = server.arg("plain");
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    Serial.println("Invalid JSON: " + String(error.c_str()));
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  if (!doc.containsKey("id")) {
    server.send(400, "text/plain", "Missing id");
    return;
  }

  String id = doc["id"].as<String>();
  if (id.length() == 0) {
    server.send(400, "text/plain", "Empty id");
    return;
  }

  int index = findSensorIndex(id);
  if (index == -1) {
    if (sensorCount >= MAX_SENSORS) {
      server.send(500, "text/plain", "Too many sensors");
      return;
    }
    index = sensorCount++;
    sensors[index] = SensorData();
    sensors[index].id = id;
  }

  SensorData& sensor = sensors[index];
  bool legacyTempHumPresent = false;
  bool nodeMultiSensorPresent = false;

  sensor.dhtCount = 0;
  for (int i = 0; i < MAX_DHT_PER_NODE; i++) {
    sensor.dhtTemp[i] = 0.0f;
    sensor.dhtHum[i] = 0.0f;
    sensor.dhtValid[i] = false;
  }

  if (doc.containsKey("temp")) {
    if (!doc["temp"].isNull()) {
      sensor.temp = doc["temp"].as<float>();
      sensor.hasTemp = true;
      legacyTempHumPresent = true;
    } else {
      sensor.temp = 0.0f;
      sensor.hasTemp = false;
    }
  }
  if (doc.containsKey("hum")) {
    if (!doc["hum"].isNull()) {
      sensor.hum = doc["hum"].as<float>();
      sensor.hasHum = true;
      legacyTempHumPresent = true;
    } else {
      sensor.hum = 0.0f;
      sensor.hasHum = false;
    }
  }
  if (doc.containsKey("dht_temp") && doc.containsKey("dht_hum") && doc["dht_temp"].is<JsonArray>() && doc["dht_hum"].is<JsonArray>()) {
    JsonArray dhtTempArray = doc["dht_temp"].as<JsonArray>();
    JsonArray dhtHumArray = doc["dht_hum"].as<JsonArray>();
    sensor.dhtCount = min(min((int)dhtTempArray.size(), (int)dhtHumArray.size()), MAX_DHT_PER_NODE);
    for (int i = 0; i < sensor.dhtCount; i++) {
      if (!dhtTempArray[i].isNull() && !dhtHumArray[i].isNull()) {
        sensor.dhtTemp[i] = dhtTempArray[i].as<float>();
        sensor.dhtHum[i] = dhtHumArray[i].as<float>();
        sensor.dhtValid[i] = true;
      }
    }
    syncLegacyTempHumFromDHT(sensor);
    if (sensor.dhtCount > 0) {
      legacyTempHumPresent = sensor.hasTemp || sensor.hasHum;
      nodeMultiSensorPresent = true;
    }
  }
  if (doc.containsKey("seq") && !doc["seq"].isNull()) {
    sensor.sequence = doc["seq"].as<uint32_t>();
    sensor.hasSequence = true;
  }

  // Legacy spectro payload path.
  int spectroData[6] = {0};
  int spectroCount = 0;
  bool lightsOn = areLightsOn();

  if (doc.containsKey("spectro") && doc["spectro"].is<JsonArray>()) {
    JsonArray spectro = doc["spectro"].as<JsonArray>();
    spectroCount = min((int)spectro.size(), 6);

    if (lightsOn) {
      sensor.spectroCount = spectroCount;
      for (int i = 0; i < spectroCount; i++) {
        spectroData[i] = spectro[i].as<int>();
        sensor.spectro[i] = spectroData[i];
      }
    } else {
      sensor.spectroCount = 0;
      spectroCount = 0;
      Serial.println("Spectro data ignored because lights are off");
    }
  } else {
    sensor.spectroCount = 0;
  }

  // Multi-lux array path. Null entries are treated as unavailable, not as zero.
  if (doc.containsKey("lux") && doc["lux"].is<JsonArray>()) {
    JsonArray luxArray = doc["lux"].as<JsonArray>();
    sensor.luxCount = min((int)luxArray.size(), MAX_LUX_PER_NODE);
    for (int i = 0; i < MAX_LUX_PER_NODE; i++) {
      sensor.luxValid[i] = false;
      sensor.lux[i] = 0.0f;
    }

    for (int i = 0; i < sensor.luxCount; i++) {
      if (!luxArray[i].isNull()) {
        sensor.lux[i] = luxArray[i].as<float>();
        sensor.luxValid[i] = true;
      }
    }
    nodeMultiSensorPresent = true;
  }

  // Moisture path is retained for future use. Current DHT11 bench-test nodes
  // send an empty moisture_raw array, which clears/keeps this count at zero.
  if (doc.containsKey("moisture_raw") && doc["moisture_raw"].is<JsonArray>()) {
    JsonArray moistureArray = doc["moisture_raw"].as<JsonArray>();
    sensor.moistureCount = min((int)moistureArray.size(), MAX_MOISTURE_PER_NODE);
    for (int i = 0; i < MAX_MOISTURE_PER_NODE; i++) {
      sensor.moistureValid[i] = false;
      sensor.moistureRaw[i] = 0;
    }

    for (int i = 0; i < sensor.moistureCount; i++) {
      if (!moistureArray[i].isNull()) {
        sensor.moistureRaw[i] = moistureArray[i].as<int>();
        sensor.moistureValid[i] = true;
      }
    }
    nodeMultiSensorPresent = true;
  }

  sensor.lastUpdated = getTimeStamp();
  sensor.lastUpdateMillis = millis();
  sensor.dataChanged = true;
  hasReceivedData = true;
  displayNeedsUpdate = true;

  if (lastLogTime == 0 && sdAvailable) {
    lastLogTime = millis();
    Serial.println("SD logging timer started - first automatic log in 1 hour");
  }

  if (legacyTempHumPresent || spectroCount > 0) {
    float tempForHistory = sensor.hasTemp ? sensor.temp : 0.0f;
    float humForHistory = sensor.hasHum ? sensor.hum : 0.0f;
    addToDataHistory(id, tempForHistory, humForHistory, spectroData, spectroCount);
  }

  if (nodeMultiSensorPresent) {
    addNodeSensorHistory(sensor);
  }

  Serial.println("Data received from " + id);
  if (sensor.hasTemp) Serial.println("  Temp(avg): " + String(sensor.temp, 2) + " C");
  if (sensor.hasHum) Serial.println("  Hum(avg): " + String(sensor.hum, 2) + " %");

  if (sensor.dhtCount > 0) {
    Serial.print("  DHT array: ");
    for (int i = 0; i < sensor.dhtCount; i++) {
      if (sensor.dhtValid[i]) {
        Serial.print("D"); Serial.print(i + 1); Serial.print("=");
        Serial.print(String(sensor.dhtTemp[i], 1)); Serial.print("C/");
        Serial.print(String(sensor.dhtHum[i], 1)); Serial.print("%");
      } else {
        Serial.print("D"); Serial.print(i + 1); Serial.print("=null");
      }
      if (i < sensor.dhtCount - 1) Serial.print(", ");
    }
    Serial.println();
  }

  if (sensor.luxCount > 0) {
    Serial.print("  Lux: ");
    for (int i = 0; i < sensor.luxCount; i++) {
      if (sensor.luxValid[i]) Serial.print(String(sensor.lux[i], 2));
      else Serial.print("null");
      if (i < sensor.luxCount - 1) Serial.print(", ");
    }
    Serial.println();
  }

  if (sensor.moistureCount > 0) {
    Serial.print("  Moisture raw: ");
    for (int i = 0; i < sensor.moistureCount; i++) {
      if (sensor.moistureValid[i]) Serial.print(sensor.moistureRaw[i]);
      else Serial.print("null");
      if (i < sensor.moistureCount - 1) Serial.print(", ");
    }
    Serial.println();
  }

  if (sensor.hasSequence) {
    Serial.println("  Seq: " + String(sensor.sequence));
  }
  Serial.println("  Time: " + sensor.lastUpdated);

  server.send(200, "text/plain", "OK");
}

// -----------------------------------------------------------------------------
// Web interface
// -----------------------------------------------------------------------------
void handleRoot() {
  String html;
  html.reserve(16000);

  html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>MushroomFarm Monitor</title><style>";
  html += "body{font-family:Arial;margin:20px;background:#f0f8ff}.container{max-width:950px;margin:auto;background:white;padding:20px;border-radius:10px}";
  html += ".status{padding:10px;margin:10px 0;border-radius:5px;background:#f8f9fa}.online{color:#155724}.offline{color:#721c24}";
  html += "button{padding:10px 14px;margin:4px;border:0;border-radius:5px;cursor:pointer}.relay-btn{background:#dc3545;color:white}.relay-btn.on{background:#28a745}";
  html += "table{width:100%;border-collapse:collapse;margin-top:12px}th,td{padding:7px;border:1px solid #ddd;text-align:center}th{background:#f8f9fa}";
  html += "</style></head><body><div class='container'><h1>MushroomFarm Monitor</h1>";

  html += "<div class='status'><b>RTC:</b> ";
  if (rtcAvailable) {
    html += "<span class='online'>Online</span> ";
    html += getDateTime();
  } else {
    html += "<span class='offline'>Offline</span>";
  }
  html += "</div>";

  html += "<div class='status'><b>SD:</b> " + String(sdAvailable ? "Active" : "Offline") +
          " &nbsp; <b>MCP23017:</b> " + String(mcpAvailable ? "Available" : "Unavailable") +
          " &nbsp; <b>Known nodes:</b> " + String(sensorCount) + "</div>";

  html += "<button onclick='syncTime()'>Sync RTC from Browser</button>";
  html += "<button onclick='resetOverrides()'>Reset Automatic Control</button>";

  html += "<h3>Manual Relay Control</h3>";
  html += "<button class='relay-btn' id='relay1' onclick='toggleRelay(1)'>Mist Maker 1</button>";
  html += "<button class='relay-btn' id='relay2' onclick='toggleRelay(2)'>Mist Maker 2</button>";
  html += "<button class='relay-btn' id='relay3' onclick='toggleRelay(3)'>Mist Maker 3</button>";
  html += "<button class='relay-btn' id='relay4' onclick='toggleRelay(4)'>Exhaust Fan</button>";
  html += "<button class='relay-btn' id='relay5' onclick='toggleRelay(5)'>Sprinkler</button>";
  html += "<button class='relay-btn' id='relay7' onclick='toggleRelay(7)'>Lights</button>";

  html += "<h3>Live Node Data - 3x DHT11 + 9x Lux</h3><table><tr><th>ID</th><th>Age</th>";
  for (int j = 0; j < MAX_DHT_PER_NODE; j++) html += "<th>DHT" + String(j + 1) + " Temp C</th><th>DHT" + String(j + 1) + " Hum %</th>";
  for (int j = 0; j < MAX_LUX_PER_NODE; j++) html += "<th>Lux " + String(j + 1) + "</th>";
  html += "<th>Seq</th></tr>";
  for (int i = 0; i < sensorCount; i++) {
    html += "<tr><td>" + sensors[i].id + "</td>";
    if (sensors[i].lastUpdateMillis > 0) html += "<td>" + String((millis() - sensors[i].lastUpdateMillis) / 1000UL) + " s</td>";
    else html += "<td>never</td>";
    for (int j = 0; j < MAX_DHT_PER_NODE; j++) {
      html += "<td>";
      if (j < sensors[i].dhtCount && sensors[i].dhtValid[j]) html += String(sensors[i].dhtTemp[j], 1); else html += "-";
      html += "</td><td>";
      if (j < sensors[i].dhtCount && sensors[i].dhtValid[j]) html += String(sensors[i].dhtHum[j], 1); else html += "-";
      html += "</td>";
    }
    for (int j = 0; j < MAX_LUX_PER_NODE; j++) {
      html += "<td>";
      if (j < sensors[i].luxCount && sensors[i].luxValid[j]) html += String(sensors[i].lux[j], 1); else html += "-";
      html += "</td>";
    }
    html += "<td>";
    html += sensors[i].hasSequence ? String(sensors[i].sequence) : String("-");
    html += "</td></tr>";
  }
  html += "</table>";

  html += R"rawliteral(
<script>
function toggleRelay(relayNum){
  const button=document.getElementById('relay'+relayNum);
  const currentlyOn=button.classList.contains('on');
  const action=currentlyOn?'off':'on';
  fetch('/relay-control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:relayNum,action:action})})
    .then(r=>r.text()).then(data=>{if(data==='OK'){button.classList.toggle('on',action==='on');}});
}
function resetOverrides(){
  fetch('/reset-overrides',{method:'POST'}).then(r=>r.text()).then(alert);
}
function syncTime(){
  const now=new Date();
  const manila=new Date(now.getTime()+(8*60*60*1000));
  const payload={year:manila.getUTCFullYear(),month:manila.getUTCMonth()+1,day:manila.getUTCDate(),hour:manila.getUTCHours(),minute:manila.getUTCMinutes(),second:manila.getUTCSeconds()};
  fetch('/sync-time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}).then(r=>r.text()).then(alert);
}
</script></div></body></html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleTimeSync() {
  if (!rtcAvailable) {
    server.send(400, "text/plain", "RTC not available");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  int year = doc["year"];
  int month = doc["month"];
  int day = doc["day"];
  int hour = doc["hour"];
  int minute = doc["minute"];
  int second = doc["second"];

  if (year < 2020 || year > 2050 || month < 1 || month > 12 ||
      day < 1 || day > 31 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) {
    server.send(400, "text/plain", "Invalid time values");
    return;
  }

  rtc.adjust(DateTime(year, month, day, hour, minute, second));
  server.send(200, "text/plain", "Time synced: " + getDateTime());
}

void handleRelayControl() {
  if (!mcpAvailable) {
    server.send(400, "text/plain", "MCP23017 not available");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  int relayNum = doc["relay"];
  String action = doc["action"].as<String>();
  bool turnOn = action == "on";

  switch (relayNum) {
    case 1:
      turnOn ? onMistMakerOne() : offMistMakerOne();
      mistMaker1ManualOverride = true;
      break;
    case 2:
      turnOn ? onMistMakerTwo() : offMistMakerTwo();
      mistMaker2ManualOverride = true;
      break;
    case 3:
      turnOn ? onMistMakerThree() : offMistMakerThree();
      mistMaker3ManualOverride = true;
      break;
    case 4:
      turnOn ? onExhaustFan() : offExhaustFan();
      exhaustFanManualOverride = true;
      break;
    case 5:
      turnOn ? onSprinklerPump() : offSprinklerPump();
      sprinklerPumpManualOverride = true;
      break;
    case 7:
      turnOn ? onLights() : offLights();
      lightsManualOverride = true;
      break;
    default:
      server.send(400, "text/plain", "Invalid relay number");
      return;
  }

  server.send(200, "text/plain", "OK");
}

void handleResetOverrides() {
  mistMaker1ManualOverride = false;
  mistMaker2ManualOverride = false;
  mistMaker3ManualOverride = false;
  exhaustFanManualOverride = false;
  sprinklerPumpManualOverride = false;
  lightsManualOverride = false;
  server.send(200, "text/plain", "Manual overrides reset; automatic control restored");
}

// -----------------------------------------------------------------------------
// TFT display
// -----------------------------------------------------------------------------
void drawSystemStatus() {
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print("WiFi: ");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.print(ssid);

  tft.setCursor(10, 35);
  if (rtcAvailable) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.print("Time: ");
    tft.print(getDateTime());
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("RTC: Not Available");
  }

  tft.setCursor(10, 60);
  if (sdAvailable) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("SD: HSPI OK");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("SD: Offline");
  }
}

void updateSystemStatus() {
  static String lastTimeDisplay = "";
  String currentTimeDisplay = rtcAvailable ? getDateTime() : "RTC: Not Available";
  if (currentTimeDisplay == lastTimeDisplay) {
    return;
  }

  tft.fillRect(0, 30, 480, 28, TFT_BLACK);
  tft.setCursor(10, 35);
  tft.setTextSize(2);
  if (rtcAvailable) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.print("Time: ");
    tft.print(getDateTime());
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("RTC: Not Available");
  }
  lastTimeDisplay = currentTimeDisplay;
}

void drawTable() {
  tft.fillRect(10, 90, 460, 190, TFT_BLACK);
  tft.drawRect(10, 90, 460, 190, TFT_WHITE);
  tft.drawLine(10, 120, 470, 120, TFT_WHITE);
  tft.drawLine(160, 90, 160, 280, TFT_WHITE);
  tft.drawLine(320, 90, 320, 280, TFT_WHITE);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(50, 95);
  tft.print("SENSOR");
  tft.setCursor(180, 95);
  tft.print("TEMP");
  tft.setCursor(355, 95);
  tft.print("HUM");
}

void displayTempHumidity() {
  tft.fillScreen(TFT_BLACK);
  drawSystemStatus();
  drawTable();

  int sortedIndices[MAX_SENSORS];
  int sortedCount;
  getSortedSensorIndices(sortedIndices, sortedCount);

  int rowY = 130;
  int rowHeight = 24;
  int displayedRows = 0;

  for (int i = 0; i < sortedCount && displayedRows < 6; i++) {
    int sensorIndex = sortedIndices[i];
    if (isSensorStale(sensorIndex)) {
      continue;
    }

    tft.setTextSize(2);
    tft.setCursor(30, rowY);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.print(sensors[sensorIndex].id);

    tft.setCursor(180, rowY);
    if (sensors[sensorIndex].hasTemp) {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.print(sensors[sensorIndex].temp, 1);
      tft.print("C");
    } else {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.print("--.-C");
    }

    tft.setCursor(350, rowY);
    if (sensors[sensorIndex].hasHum) {
      tft.setTextColor(TFT_BLUE, TFT_BLACK);
      tft.print(sensors[sensorIndex].hum, 1);
      tft.print("%");
    } else {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.print("--.-%");
    }

    if (displayedRows > 0) {
      tft.drawLine(10, rowY - 4, 470, rowY - 4, TFT_DARKGREY);
    }

    rowY += rowHeight;
    displayedRows++;
  }

  if (displayedRows == 0) {
    tft.setCursor(100, 180);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("No temp/hum data");
  }

  tft.fillRect(0, 285, 480, 35, TFT_NAVY);
  tft.setCursor(20, 295);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.print("Button: next page");
}

void updateSensorRow(int sensorIndex, int rowY) {
  if (sensorIndex < 0 || sensorIndex >= sensorCount) {
    return;
  }

  tft.fillRect(11, rowY - 2, 458, 24, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, rowY);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print(sensors[sensorIndex].id);

  tft.setCursor(180, rowY);
  if (sensors[sensorIndex].hasTemp) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print(sensors[sensorIndex].temp, 1);
    tft.print("C");
  } else {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.print("--.-C");
  }

  tft.setCursor(350, rowY);
  if (sensors[sensorIndex].hasHum) {
    tft.setTextColor(TFT_BLUE, TFT_BLACK);
    tft.print(sensors[sensorIndex].hum, 1);
    tft.print("%");
  } else {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.print("--.-%");
  }

  sensors[sensorIndex].dataChanged = false;
}

void displaySpectroReadings() {
  tft.fillScreen(TFT_BLACK);
  drawSystemStatus();

  if (!areLightsOn()) {
    tft.drawRect(10, 90, 460, 190, TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(65, 155);
    tft.print("Spectro Collection Disabled");
    tft.setCursor(85, 190);
    tft.print("Lights are OFF");
  } else {
    tft.fillRect(10, 90, 460, 190, TFT_BLACK);
    tft.drawRect(10, 90, 460, 190, TFT_WHITE);
    tft.drawLine(10, 120, 470, 120, TFT_WHITE);

    int colWidth = 460 / 5;
    for (int i = 1; i < 5; i++) {
      int x = 10 + i * colWidth;
      tft.drawLine(x, 90, x, 280, TFT_WHITE);
    }

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int i = 0; i < 5; i++) {
      tft.setCursor(20 + i * colWidth, 98);
      tft.printf("Treatment%d", i + 1);
    }

    int rowY = 130;
    displayTreatmentColumn(0, colWidth, rowY, 1, "sensor1", 0, 2, TFT_GREEN);
    displayTreatmentColumn(1, colWidth, rowY, 2, "sensor1", 3, 5, TFT_GREEN);
    displayTreatmentColumn(2, colWidth, rowY, 3, "sensor2", 0, 2, TFT_GREEN);
    displayTreatmentColumn(3, colWidth, rowY, 4, "sensor2", 3, 5, TFT_CYAN);
    displayTreatmentColumn(4, colWidth, rowY, 5, "sensor3", 0, 2, TFT_RED);
  }

  tft.fillRect(0, 285, 480, 35, TFT_NAVY);
  tft.setCursor(20, 295);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.print("Button: next page");
}

void displayTreatmentColumn(int columnIndex, int colWidth, int startY, int treatmentNum,
                            const char* sensorId, int startIdx, int endIdx,
                            uint16_t textColor) {
  (void)treatmentNum;
  int columnX = 10 + columnIndex * colWidth;
  int sensorIndex = findSensorIndex(String(sensorId));

  if (sensorIndex == -1) {
    tft.setCursor(columnX + 8, startY + 20);
    tft.setTextSize(1);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("No Sensor");
    return;
  }

  if (isSensorStale(sensorIndex)) {
    tft.setCursor(columnX + 12, startY + 20);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.print("Timeout");
    return;
  }

  if (sensors[sensorIndex].spectroCount <= 0) {
    tft.setCursor(columnX + 12, startY + 20);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.print("No Data");
    return;
  }

  int valueY = startY;
  tft.setTextSize(1);
  tft.setTextColor(textColor, TFT_BLACK);
  for (int i = startIdx; i <= endIdx && i < sensors[sensorIndex].spectroCount; i++) {
    tft.setCursor(columnX + 6, valueY);
    tft.printf("%d", sensors[sensorIndex].spectro[i]);
    valueY += 15;
  }
}

void clearTreatmentColumn(int columnIndex, int colWidth, int startY) {
  int columnX = 10 + columnIndex * colWidth;
  tft.fillRect(columnX + 1, startY, colWidth - 2, 130, TFT_BLACK);
}

void updateTreatmentData() {
  // Keeping this simple and deterministic: redraw the spectro page when new
  // spectro data arrives. This preserves the visible behavior without relying
  // on Word-document-fragmented partial-update code.
  displaySpectroReadings();
  for (int i = 0; i < sensorCount; i++) {
    sensors[i].dataChanged = false;
  }
}

void displayNodeSensors() {
  tft.fillScreen(TFT_BLACK);
  drawSystemStatus();
  tft.fillRect(10, 90, 460, 190, TFT_BLACK);
  tft.drawRect(10, 90, 460, 190, TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(16, 96);
  tft.print("NODE DETAIL: 3x DHT11 + 9x LUX");
  tft.drawLine(10, 114, 470, 114, TFT_WHITE);

  int sortedIndices[MAX_SENSORS];
  int sortedCount;
  getSortedSensorIndices(sortedIndices, sortedCount);
  int selected = -1;
  for (int i = 0; i < sortedCount; i++) {
    int index = sortedIndices[i];
    bool hasNodePayload = sensors[index].luxCount > 0 || sensors[index].dhtCount > 0 || sensors[index].hasTemp || sensors[index].hasHum || sensors[index].moistureCount > 0;
    if (hasNodePayload && !isSensorStale(index)) { selected = index; break; }
  }
  if (selected == -1) {
    tft.setTextSize(2); tft.setTextColor(TFT_RED, TFT_BLACK); tft.setCursor(70, 175); tft.print("No DHT11/lux data");
    tft.setTextSize(1); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.setCursor(16, 268); tft.print("Moisture support retained in firmware; disabled for this test.");
    tft.fillRect(0, 285, 480, 35, TFT_NAVY); tft.setTextColor(TFT_WHITE, TFT_NAVY); tft.setCursor(10, 296); tft.print("Page 3/3  Button cycles pages");
    return;
  }
  SensorData& s = sensors[selected];
  int y = 122;
  tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.setCursor(16, y); tft.print("Node: "); tft.print(s.id);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setCursor(160, y); tft.print("Age: "); tft.print((millis() - s.lastUpdateMillis) / 1000UL); tft.print("s");
  tft.setCursor(300, y); tft.print("Seq: "); if (s.hasSequence) tft.print(s.sequence); else tft.print("-");
  y += 18;
  for (int i = 0; i < MAX_DHT_PER_NODE; i++) {
    tft.setCursor(16, y); tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.print("DHT"); tft.print(i + 1); tft.print(": ");
    if (i < s.dhtCount && s.dhtValid[i]) { tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.print(s.dhtTemp[i], 1); tft.print("C  "); tft.print(s.dhtHum[i], 1); tft.print("%"); }
    else { tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.print("--"); }
    y += 16;
  }
  y += 4;
  for (int row = 0; row < 3; row++) {
    tft.setCursor(16, y);
    for (int col = 0; col < 3; col++) {
      int idx = row * 3 + col;
      tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.print("L"); tft.print(idx + 1); tft.print(":");
      if (idx < s.luxCount && s.luxValid[idx]) { tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.print(s.lux[idx], 1); }
      else { tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.print("--"); }
      if (col < 2) tft.print("   ");
    }
    y += 16;
  }
  tft.setTextSize(1); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.setCursor(16, 268); tft.print("Moisture support retained in firmware; disabled for this test.");
  tft.fillRect(0, 285, 480, 35, TFT_NAVY); tft.setTextColor(TFT_WHITE, TFT_NAVY); tft.setCursor(10, 296); tft.print("Page 3/3  Button cycles pages");
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Starting Mushroom Farm Monitor - ZY-ESP32 MAIN");

  Wire.begin(21, 22);

  // Separate HSPI bus for SD, matching the legacy architecture.
  hspi.begin(14, 12, 13, 15);
  pinMode(5, OUTPUT);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(5, HIGH);
  digitalWrite(SD_CS_PIN, HIGH);

  // TFT through TFT_eSPI / VSPI.
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(50, 80);
  tft.println("MushroomFarm");
  tft.setTextSize(2);
  tft.setCursor(80, 120);
  tft.println("ZY-ESP32 Main");

  if (rtc.begin()) {
    rtcAvailable = true;
    Serial.println("DS3231 RTC initialized");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power; use SET_TIME or web time sync");
    }
  } else {
    rtcAvailable = false;
    Serial.println("DS3231 RTC not found; continuing without RTC");
  }

  sdAvailable = initializeSDCard();
  Serial.println(String("SD: ") + (sdAvailable ? "ready" : "offline"));

  if (mcp.begin_I2C(0x20)) {
    mcpAvailable = true;
    mcp.pinMode(RELAY1, OUTPUT);
    mcp.pinMode(RELAY2, OUTPUT);
    mcp.pinMode(RELAY3, OUTPUT);
    mcp.pinMode(EXHAUST_FAN, OUTPUT);
    mcp.pinMode(SPRINKLER_PUMP, OUTPUT);
    mcp.pinMode(LIGHTS, OUTPUT);
    mcp.pinMode(RELAY_C3_POWER, OUTPUT);

    mcp.digitalWrite(RELAY1, HIGH);
    mcp.digitalWrite(RELAY2, HIGH);
    mcp.digitalWrite(RELAY3, HIGH);
    mcp.digitalWrite(EXHAUST_FAN, HIGH);
    mcp.digitalWrite(SPRINKLER_PUMP, HIGH);
    mcp.digitalWrite(LIGHTS, HIGH);
    mcp.digitalWrite(RELAY_C3_POWER, HIGH);
    Serial.println("MCP23017 initialized; all relays OFF");
  } else {
    mcpAvailable = false;
    Serial.println("MCP23017 not found; relay control disabled");
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password, 1, 0, 8);
  delay(500);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/data", HTTP_POST, handleData);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/sync-time", HTTP_POST, handleTimeSync);
  server.on("/relay-control", HTTP_POST, handleRelayControl);
  server.on("/reset-overrides", HTTP_POST, handleResetOverrides);
  server.begin();

  tft.setTextSize(2);
  tft.setCursor(20, 165);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.print("AP: ");
  tft.println(ssid);
  tft.setCursor(20, 190);
  tft.print("IP: ");
  tft.println(WiFi.softAPIP());
  tft.setCursor(20, 215);
  tft.setTextColor(sdAvailable ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.println(sdAvailable ? "SD: OK" : "SD: OFFLINE");
  tft.setCursor(20, 240);
  tft.setTextColor(mcpAvailable ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.println(mcpAvailable ? "MCP23017: OK" : "MCP23017: OFFLINE");

  delay(2500);

  displayNeedsUpdate = true;
  displayInitialized = false;

  Serial.println("Expected current test node: sensor1");
  Serial.println("POST endpoint: http://192.168.4.1/data");
  Serial.println("Display pages: Temp/Hum -> Spectro -> Node Sensors");
  Serial.println("Commands: LOG_NOW, BUFFER_STATUS, LIST_FILES, SET_TIME, RESET_OVERRIDES, HELP");
}

// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------
void loop() {
  server.handleClient();
  controlAllRelays();

  // Serial maintenance interface.
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    String upper = command;
    upper.toUpperCase();

    if (upper == "LOG_NOW") {
      // Explicit legacy maintenance test only. This does not run automatically.
      if (sensorCount == 0) {
        Serial.println("LOG_NOW: no sensor data; creating legacy logging test data");
        sensorCount = 4;
        const float testTemp[4] = {23.5f, 24.1f, 23.8f, 24.2f};
        const float testHum[4] = {85.2f, 87.8f, 86.7f, 88.1f};
        for (int i = 0; i < 4; i++) {
          sensors[i] = SensorData();
          sensors[i].id = "sensor" + String(i + 1);
          sensors[i].temp = testTemp[i];
          sensors[i].hum = testHum[i];
          sensors[i].hasTemp = true;
          sensors[i].hasHum = true;
          sensors[i].lastUpdated = "12:34:56";
        }
        sensors[0].spectroCount = 6;
        sensors[1].spectroCount = 6;
        sensors[3].spectroCount = 6;
        for (int i = 0; i < 6; i++) {
          sensors[0].spectro[i] = 1000 + i;
          sensors[1].spectro[i] = 2000 + i;
          sensors[3].spectro[i] = 4000 + i;
        }

        int dummy0[6];
        int dummy1[6];
        int dummy3[6];
        for (int i = 0; i < 6; i++) {
          dummy0[i] = sensors[0].spectro[i];
          dummy1[i] = sensors[1].spectro[i];
          dummy3[i] = sensors[3].spectro[i];
        }
        addToDataHistory("sensor1", testTemp[0], testHum[0], dummy0, 6);
        addToDataHistory("sensor2", testTemp[1], testHum[1], dummy1, 6);
        addToDataHistory("sensor3", testTemp[2], testHum[2], nullptr, 0);
        addToDataHistory("sensor4", testTemp[3], testHum[3], dummy3, 6);
      }
      logSensorDataToCSV();
    }
    else if (upper.startsWith("SET_TIME ")) {
      String args = command.substring(9);
      int values[6] = {0};
      int count = 0;
      int start = 0;
      for (int i = 0; i <= args.length() && count < 6; i++) {
        if (i == args.length() || args.charAt(i) == ' ') {
          if (i > start) {
            values[count++] = args.substring(start, i).toInt();
          }
          start = i + 1;
        }
      }
      if (count == 6) {
        setRTCTime(values[0], values[1], values[2], values[3], values[4], values[5]);
      } else {
        Serial.println("Use: SET_TIME YYYY MM DD HH MM SS");
      }
    }
    else if (upper == "LIST_FILES") {
      if (sdAvailable) listSDCardFiles();
      else Serial.println("SD unavailable");
    }
    else if (upper == "BUFFER_STATUS") {
      Serial.println("Legacy history entries: " + String(historyCount) + "/" + String(MAX_HISTORY_ENTRIES));
      Serial.println("Node history entries: " + String(nodeHistoryCount) + "/" + String(MAX_NODE_HISTORY_ENTRIES));
      Serial.println("Known sensors: " + String(sensorCount));
    }
    else if (upper == "RESET_OVERRIDES") {
      mistMaker1ManualOverride = false;
      mistMaker2ManualOverride = false;
      mistMaker3ManualOverride = false;
      exhaustFanManualOverride = false;
      sprinklerPumpManualOverride = false;
      lightsManualOverride = false;
      Serial.println("Manual overrides reset");
    }
    else if (upper == "HELP") {
      Serial.println("LOG_NOW");
      Serial.println("BUFFER_STATUS");
      Serial.println("LIST_FILES");
      Serial.println("SET_TIME YYYY MM DD HH MM SS");
      Serial.println("RESET_OVERRIDES");
      Serial.println("HELP");
      Serial.println("Web: http://192.168.4.1");
    }
  }

  handleButton();

  static unsigned long lastStaleCheck = 0;
  if (millis() - lastStaleCheck >= 30000UL) {
    clearStaleSensors();
    lastStaleCheck = millis();
  }

  monitorC3AndCycleRelay();

  static bool lastLightsState = false;
  bool currentLightsState = areLightsOn();
  if (displayMode == MODE_SPECTRO && displayInitialized && currentLightsState != lastLightsState) {
    displayNeedsUpdate = true;
    displayInitialized = false;
  }
  lastLightsState = currentLightsState;

  if (displayNeedsUpdate) {
    if (!displayInitialized || currentDisplayMode != displayMode) {
      if (displayMode == MODE_TEMP_HUM) {
        displayTempHumidity();
      } else if (displayMode == MODE_SPECTRO) {
        displaySpectroReadings();
      } else {
        displayNodeSensors();
      }
      displayInitialized = true;
      currentDisplayMode = displayMode;
    } else {
      if (displayMode == MODE_TEMP_HUM) {
        int sortedIndices[MAX_SENSORS];
        int sortedCount;
        getSortedSensorIndices(sortedIndices, sortedCount);
        int rowY = 130;
        int displayedRows = 0;
        for (int i = 0; i < sortedCount && displayedRows < 6; i++) {
          int sensorIndex = sortedIndices[i];
          if (isSensorStale(sensorIndex)) continue;
          if (sensors[sensorIndex].dataChanged) {
            updateSensorRow(sensorIndex, rowY);
          }
          rowY += 24;
          displayedRows++;
        }
      } else if (displayMode == MODE_SPECTRO) {
        updateTreatmentData();
      } else {
        // Current node page is small; full redraw is simpler and reliable.
        displayNodeSensors();
        for (int i = 0; i < sensorCount; i++) sensors[i].dataChanged = false;
      }
    }

    updateSystemStatus();
    displayNeedsUpdate = false;
  }

  unsigned long currentTime = millis();
  if (displayInitialized && currentTime - lastTimeUpdate >= TIME_UPDATE_INTERVAL) {
    updateSystemStatus();
    lastTimeUpdate = currentTime;
  }

  if (lastLogTime > 0 && currentTime - lastLogTime >= LOG_INTERVAL) {
    if (sdAvailable && (historyCount > 0 || nodeHistoryCount > 0)) {
      logSensorDataToCSV();
    } else if (!sdAvailable) {
      Serial.println("Automatic log skipped: SD unavailable");
    } else {
      Serial.println("Automatic log skipped: no buffered data");
    }
    lastLogTime = currentTime;
  }

  delay(50);
}
