/*
 * MushroomFarm Sensor Node 1 - Serial Debug Build
 * Target: ESP32-C3 SuperMini / ESP32-C3 Mini development board
 * Node ID sent to main: sensor1
 *
 * CURRENT BENCH TEST:
 *   - 9 x VEML7700 lux sensors through 2 x TCA9548A multiplexers
 *     * Lux 1..8 -> TCA9548A #1 at 0x70, channels 0..7
 *     * Lux 9    -> TCA9548A #2 at 0x71, channel 0
 *   - 3 x DHT11 temperature + humidity sensors
 *     * DHT11 #1 -> GPIO6
 *     * DHT11 #2 -> GPIO0
 *     * DHT11 #3 -> GPIO1
 *   - Moisture sensing code is retained but disabled for this test
 *
 * SERIAL MONITOR:
 *   Baud: 115200
 *   Commands: help, status, wifi, scan, muxscan, sensors, read, post,
 *             initlux, debug on, debug off, reboot
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <DHT.h>

// -----------------------------------------------------------------------------
// Identity / network
// -----------------------------------------------------------------------------
const char* NODE_ID = "sensor1";
const char* MAIN_AP_SSID = "MushrooMan";
const char* MAIN_AP_PASSWORD = "RABE2026qt";
const char* MAIN_DATA_URL = "http://192.168.4.1/data";

// -----------------------------------------------------------------------------
// ESP32-C3 pin map
// -----------------------------------------------------------------------------
const uint8_t I2C_SDA_PIN = 4;
const uint8_t I2C_SCL_PIN = 5;

const uint8_t DHT_PINS[3] = {6, 0, 1};
#define DHT_TYPE DHT11
DHT dht1(DHT_PINS[0], DHT_TYPE);
DHT dht2(DHT_PINS[1], DHT_TYPE);
DHT dht3(DHT_PINS[2], DHT_TYPE);
DHT* dhtSensors[3] = {&dht1, &dht2, &dht3};

// Moisture support retained, but disabled.
const uint8_t MOISTURE_PINS[3] = {2, 3, 7};
const bool ENABLE_MOISTURE = false;

// -----------------------------------------------------------------------------
// Sensor topology
// -----------------------------------------------------------------------------
const uint8_t MAX_LUX_SENSORS = 9;
const uint8_t MAX_DHT_SENSORS = 3;
const uint8_t MAX_MOISTURE_SENSORS = 3;
const uint8_t ACTIVE_LUX_SENSORS = 9;
const uint8_t ACTIVE_DHT_SENSORS = 3;
const uint8_t CONFIGURED_MOISTURE_SENSORS = 3;

struct LuxRoute {
  uint8_t muxAddress;
  uint8_t channel;
};

// Current 8 + 1 distribution. Change only this table if the physical routing changes.
const LuxRoute LUX_ROUTES[MAX_LUX_SENSORS] = {
  {0x70, 0}, {0x70, 1}, {0x70, 2}, {0x70, 3},
  {0x70, 4}, {0x70, 5}, {0x70, 6}, {0x70, 7},
  {0x71, 0}
};

Adafruit_VEML7700 veml;
bool luxAvailable[MAX_LUX_SENSORS] = {false};

// -----------------------------------------------------------------------------
// Timing / diagnostics
// -----------------------------------------------------------------------------
const unsigned long POST_INTERVAL_MS = 5000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000UL;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
const unsigned long HEARTBEAT_INTERVAL_MS = 15000UL;

unsigned long lastPostMillis = 0;
unsigned long lastWifiAttemptMillis = 0;
unsigned long lastHeartbeatMillis = 0;
uint32_t sequenceNumber = 0;

bool debugEnabled = true;
uint32_t postAttempts = 0;
uint32_t postSuccesses = 0;
uint32_t postFailures = 0;
int lastHttpCode = 0;

// Cache last real readings so Serial commands can show them without inventing data.
float lastLux[MAX_LUX_SENSORS] = {0};
bool lastLuxValid[MAX_LUX_SENSORS] = {false};
float lastDhtTemp[MAX_DHT_SENSORS] = {0};
float lastDhtHum[MAX_DHT_SENSORS] = {0};
bool lastDhtValid[MAX_DHT_SENSORS] = {false};

// -----------------------------------------------------------------------------
// Small debug helpers
// -----------------------------------------------------------------------------
void debugLine(const String& msg) {
  if (debugEnabled) Serial.println(msg);
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "SSID_NOT_FOUND";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

bool probeAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// -----------------------------------------------------------------------------
// TCA9548A helpers
// -----------------------------------------------------------------------------
bool tcaSelect(uint8_t address, uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(address);
  Wire.write(1U << channel);
  uint8_t result = Wire.endTransmission();

  if (debugEnabled && result != 0) {
    Serial.printf("[I2C] TCA 0x%02X CH%u select failed, Wire error=%u\n",
                  address, channel, result);
  }
  return result == 0;
}

void tcaDisableAll(uint8_t address) {
  Wire.beginTransmission(address);
  Wire.write(0x00);
  Wire.endTransmission();
}

void disableAllMuxChannels() {
  tcaDisableAll(0x70);
  tcaDisableAll(0x71);
}

// -----------------------------------------------------------------------------
// I2C / mux diagnostics
// -----------------------------------------------------------------------------
void scanI2CBus() {
  disableAllMuxChannels();
  Serial.println();
  Serial.println("[SCAN] Upstream I2C bus scan");
  Serial.printf("[SCAN] SDA=GPIO%u, SCL=GPIO%u, clock=100kHz\n", I2C_SDA_PIN, I2C_SCL_PIN);

  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("[SCAN] Found device at 0x%02X", address);
      if (address == 0x70) Serial.print("  <- TCA9548A #1 expected");
      if (address == 0x71) Serial.print("  <- TCA9548A #2 expected");
      Serial.println();
      count++;
    }
  }
  Serial.printf("[SCAN] %u upstream device(s) found\n", count);
}

void scanMuxChannels() {
  Serial.println();
  Serial.println("[MUXSCAN] Checking configured lux routes");

  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    const LuxRoute& route = LUX_ROUTES[i];
    disableAllMuxChannels();

    Serial.printf("[MUXSCAN] Lux%u -> TCA 0x%02X CH%u: ",
                  i + 1, route.muxAddress, route.channel);

    if (!probeAddress(route.muxAddress)) {
      Serial.println("MUX NOT FOUND");
      continue;
    }
    if (!tcaSelect(route.muxAddress, route.channel)) {
      Serial.println("CHANNEL SELECT FAILED");
      continue;
    }
    delay(3);

    if (probeAddress(0x10)) {
      Serial.println("VEML7700 FOUND at 0x10");
    } else {
      Serial.println("NO VEML7700 at 0x10");
    }
  }

  disableAllMuxChannels();
}

// -----------------------------------------------------------------------------
// VEML7700 initialization / reads
// -----------------------------------------------------------------------------
void initializeLuxSensors() {
  Serial.println();
  Serial.println("[LUX] Initializing VEML7700 sensors...");

  uint8_t detected = 0;
  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    const LuxRoute& route = LUX_ROUTES[i];
    luxAvailable[i] = false;

    if (!tcaSelect(route.muxAddress, route.channel)) {
      Serial.printf("[LUX] Lux%u FAIL - TCA 0x%02X CH%u unavailable\n",
                    i + 1, route.muxAddress, route.channel);
      continue;
    }

    delay(5);
    if (!veml.begin()) {
      Serial.printf("[LUX] Lux%u FAIL - VEML7700 not detected (0x%02X CH%u)\n",
                    i + 1, route.muxAddress, route.channel);
      continue;
    }

    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    luxAvailable[i] = true;
    detected++;

    Serial.printf("[LUX] Lux%u OK   - TCA 0x%02X CH%u\n",
                  i + 1, route.muxAddress, route.channel);
  }

  disableAllMuxChannels();
  Serial.printf("[LUX] Initialization complete: %u/%u detected\n",
                detected, ACTIVE_LUX_SENSORS);
}

bool readLuxSensor(uint8_t index, float& luxOut) {
  if (index >= ACTIVE_LUX_SENSORS) return false;

  if (!luxAvailable[index]) {
    return false;
  }

  const LuxRoute& route = LUX_ROUTES[index];
  if (!tcaSelect(route.muxAddress, route.channel)) {
    luxAvailable[index] = false;
    return false;
  }

  delay(3);
  luxOut = veml.readLux();
  bool valid = !isnan(luxOut);

  if (!valid && debugEnabled) {
    Serial.printf("[LUX] Lux%u read returned NaN\n", index + 1);
  }
  return valid;
}

// -----------------------------------------------------------------------------
// DHT11
// -----------------------------------------------------------------------------
void beginDHTSensors() {
  Serial.println("[DHT] Starting 3 DHT11 sensors...");
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    dhtSensors[i]->begin();
    Serial.printf("[DHT] DHT%u DATA=GPIO%u initialized\n", i + 1, DHT_PINS[i]);
    delay(50);
  }
}

void readAllDHT11(float tempC[], float humidity[], bool valid[]) {
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    humidity[i] = dhtSensors[i]->readHumidity();
    tempC[i] = dhtSensors[i]->readTemperature();
    valid[i] = !(isnan(tempC[i]) || isnan(humidity[i]));

    if (valid[i]) {
      if (debugEnabled) {
        Serial.printf("[DHT] DHT%u GPIO%u = %.1f C / %.1f %%\n",
                      i + 1, DHT_PINS[i], tempC[i], humidity[i]);
      }
    } else {
      Serial.printf("[DHT] DHT%u GPIO%u READ FAILED\n", i + 1, DHT_PINS[i]);
      tempC[i] = 0.0f;
      humidity[i] = 0.0f;
    }
  }
}

// -----------------------------------------------------------------------------
// Moisture - retained for future use
// -----------------------------------------------------------------------------
int readMoistureRaw(uint8_t pin) {
  const uint8_t samples = 12;
  uint32_t total = 0;
  for (uint8_t i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return static_cast<int>(total / samples);
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------
void printWiFiStatus() {
  Serial.println();
  Serial.println("[WIFI] ---------- STATUS ----------");
  Serial.printf("[WIFI] State: %s (%d)\n", wifiStatusName(WiFi.status()), WiFi.status());
  Serial.printf("[WIFI] Target SSID: %s\n", MAIN_AP_SSID);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected SSID: %s\n", WiFi.SSID().c_str());
    Serial.print("[WIFI] Node IP: "); Serial.println(WiFi.localIP());
    Serial.print("[WIFI] Gateway/Main AP IP: "); Serial.println(WiFi.gatewayIP());
    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
    Serial.print("[WIFI] Node MAC: "); Serial.println(WiFi.macAddress());
  }
  Serial.printf("[WIFI] POST URL: %s\n", MAIN_DATA_URL);
  Serial.println("[WIFI] ----------------------------");
}

bool connectToMainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println();
  Serial.printf("[WIFI] Connecting to main AP '%s'...\n", MAIN_AP_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(MAIN_AP_SSID, MAIN_AP_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] CONNECTED to main AP");
    printWiFiStatus();
    return true;
  }

  Serial.printf("[WIFI] Connection timed out. Final state: %s\n",
                wifiStatusName(WiFi.status()));
  return false;
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMillis < WIFI_RETRY_INTERVAL_MS) return;

  lastWifiAttemptMillis = millis();
  Serial.printf("[WIFI] Link down (%s). Retrying...\n", wifiStatusName(WiFi.status()));
  WiFi.disconnect(true);
  delay(50);
  connectToMainWiFi();
}

// -----------------------------------------------------------------------------
// Sensor read + payload
// -----------------------------------------------------------------------------
String buildPayload() {
  Serial.println();
  Serial.printf("[READ] ===== Sample cycle, next seq=%lu =====\n",
                static_cast<unsigned long>(sequenceNumber + 1));

  float luxValues[MAX_LUX_SENSORS] = {0};
  bool luxValid[MAX_LUX_SENSORS] = {false};

  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    luxValid[i] = readLuxSensor(i, luxValues[i]);
    lastLuxValid[i] = luxValid[i];
    if (luxValid[i]) {
      lastLux[i] = luxValues[i];
      if (debugEnabled) {
        Serial.printf("[LUX] Lux%u = %.2f lx (0x%02X CH%u)\n",
                      i + 1, luxValues[i], LUX_ROUTES[i].muxAddress,
                      LUX_ROUTES[i].channel);
      }
    } else if (debugEnabled) {
      Serial.printf("[LUX] Lux%u = unavailable (0x%02X CH%u)\n",
                    i + 1, LUX_ROUTES[i].muxAddress, LUX_ROUTES[i].channel);
    }
  }
  disableAllMuxChannels();

  float dhtTemp[MAX_DHT_SENSORS] = {0};
  float dhtHum[MAX_DHT_SENSORS] = {0};
  bool dhtValid[MAX_DHT_SENSORS] = {false};
  readAllDHT11(dhtTemp, dhtHum, dhtValid);

  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    lastDhtValid[i] = dhtValid[i];
    if (dhtValid[i]) {
      lastDhtTemp[i] = dhtTemp[i];
      lastDhtHum[i] = dhtHum[i];
    }
  }

  int moistureValues[MAX_MOISTURE_SENSORS] = {0};
  if (ENABLE_MOISTURE) {
    for (uint8_t i = 0; i < CONFIGURED_MOISTURE_SENSORS; i++) {
      moistureValues[i] = readMoistureRaw(MOISTURE_PINS[i]);
    }
  }

  float tempSum = 0.0f;
  float humSum = 0.0f;
  uint8_t dhtValidCount = 0;
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    if (dhtValid[i]) {
      tempSum += dhtTemp[i];
      humSum += dhtHum[i];
      dhtValidCount++;
    }
  }

  bool hasAverage = dhtValidCount > 0;
  float avgTemp = hasAverage ? tempSum / dhtValidCount : 0.0f;
  float avgHum = hasAverage ? humSum / dhtValidCount : 0.0f;

  if (debugEnabled) {
    if (hasAverage) {
      Serial.printf("[DHT] Valid sensors=%u/%u, average=%.1f C / %.1f %%\n",
                    dhtValidCount, ACTIVE_DHT_SENSORS, avgTemp, avgHum);
    } else {
      Serial.println("[DHT] No valid DHT11 readings this cycle");
    }
  }

  sequenceNumber++;

  String payload;
  payload.reserve(1024);
  payload += "{\"id\":\"";
  payload += NODE_ID;
  payload += "\",\"seq\":";
  payload += String(sequenceNumber);
  payload += ",\"uptime_ms\":";
  payload += String(millis());
  payload += ",\"temp\":";
  payload += hasAverage ? String(avgTemp, 1) : String("null");
  payload += ",\"hum\":";
  payload += hasAverage ? String(avgHum, 1) : String("null");

  payload += ",\"dht_temp\":[";
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    payload += dhtValid[i] ? String(dhtTemp[i], 1) : String("null");
    if (i < ACTIVE_DHT_SENSORS - 1) payload += ',';
  }
  payload += ']';

  payload += ",\"dht_hum\":[";
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    payload += dhtValid[i] ? String(dhtHum[i], 1) : String("null");
    if (i < ACTIVE_DHT_SENSORS - 1) payload += ',';
  }
  payload += ']';

  payload += ",\"lux\":[";
  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    payload += luxValid[i] ? String(luxValues[i], 2) : String("null");
    if (i < ACTIVE_LUX_SENSORS - 1) payload += ',';
  }
  payload += ']';

  payload += ",\"moisture_raw\":[";
  if (ENABLE_MOISTURE) {
    for (uint8_t i = 0; i < CONFIGURED_MOISTURE_SENSORS; i++) {
      payload += String(moistureValues[i]);
      if (i < CONFIGURED_MOISTURE_SENSORS - 1) payload += ',';
    }
  }
  payload += "]}";

  if (debugEnabled) {
    Serial.println("[JSON] Payload:");
    Serial.println(payload);
  }

  return payload;
}

// -----------------------------------------------------------------------------
// HTTP POST
// -----------------------------------------------------------------------------
bool postToMain(const String& payload) {
  postAttempts++;

  if (WiFi.status() != WL_CONNECTED) {
    postFailures++;
    lastHttpCode = 0;
    Serial.println("[HTTP] POST skipped: Wi-Fi is not connected");
    return false;
  }

  Serial.printf("[HTTP] POST #%lu -> %s\n",
                static_cast<unsigned long>(postAttempts), MAIN_DATA_URL);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);

  if (!http.begin(MAIN_DATA_URL)) {
    postFailures++;
    lastHttpCode = 0;
    Serial.println("[HTTP] http.begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  unsigned long start = millis();
  int responseCode = http.POST(payload);
  unsigned long elapsed = millis() - start;
  lastHttpCode = responseCode;

  if (responseCode > 0) {
    String response = http.getString();
    Serial.printf("[HTTP] Response: %d in %lu ms, body='%s'\n",
                  responseCode, elapsed, response.c_str());

    if (responseCode == 200) {
      postSuccesses++;
    } else {
      postFailures++;
    }
  } else {
    postFailures++;
    Serial.printf("[HTTP] POST failed after %lu ms: %s\n",
                  elapsed, http.errorToString(responseCode).c_str());
  }

  http.end();
  return responseCode == 200;
}

// -----------------------------------------------------------------------------
// Serial command interface
// -----------------------------------------------------------------------------
void printHelp() {
  Serial.println();
  Serial.println("========== SERIAL DEBUG COMMANDS ==========");
  Serial.println("help       - show this command list");
  Serial.println("status     - node, uptime, counters, sensor summary");
  Serial.println("wifi       - detailed Wi-Fi/main connection status");
  Serial.println("scan       - scan upstream I2C bus");
  Serial.println("muxscan    - check VEML7700 at every configured mux route");
  Serial.println("sensors    - show cached sensor state/readings");
  Serial.println("read       - take one sensor sample and print JSON; do not POST");
  Serial.println("post       - take one sample and POST it immediately");
  Serial.println("initlux    - re-detect/reinitialize all VEML7700 sensors");
  Serial.println("debug on   - verbose per-sensor output ON");
  Serial.println("debug off  - verbose per-sensor output OFF");
  Serial.println("reboot     - restart ESP32-C3");
  Serial.println("===========================================");
}

void printCachedSensors() {
  Serial.println();
  Serial.println("[SENSORS] ---------- CACHED READINGS ----------");
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    Serial.printf("[SENSORS] DHT%u GPIO%u: ", i + 1, DHT_PINS[i]);
    if (lastDhtValid[i]) {
      Serial.printf("%.1f C / %.1f %%\n", lastDhtTemp[i], lastDhtHum[i]);
    } else {
      Serial.println("unavailable / no valid reading yet");
    }
  }

  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    Serial.printf("[SENSORS] Lux%u [TCA 0x%02X CH%u]: init=%s, reading=",
                  i + 1, LUX_ROUTES[i].muxAddress, LUX_ROUTES[i].channel,
                  luxAvailable[i] ? "OK" : "FAIL");
    if (lastLuxValid[i]) {
      Serial.printf("%.2f lx\n", lastLux[i]);
    } else {
      Serial.println("unavailable / no valid reading yet");
    }
  }
  Serial.println("[SENSORS] -------------------------------------");
}

void printStatus() {
  Serial.println();
  Serial.println("[STATUS] ========== NODE STATUS ==========");
  Serial.printf("[STATUS] Node ID: %s\n", NODE_ID);
  Serial.printf("[STATUS] Uptime: %lu ms\n", static_cast<unsigned long>(millis()));
  Serial.printf("[STATUS] Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[STATUS] Sequence: %lu\n", static_cast<unsigned long>(sequenceNumber));
  Serial.printf("[STATUS] Debug verbose: %s\n", debugEnabled ? "ON" : "OFF");
  Serial.printf("[STATUS] Wi-Fi: %s\n", wifiStatusName(WiFi.status()));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[STATUS] RSSI: %d dBm\n", WiFi.RSSI());
    Serial.print("[STATUS] Node IP: "); Serial.println(WiFi.localIP());
  }
  Serial.printf("[STATUS] HTTP attempts/success/fail: %lu/%lu/%lu\n",
                static_cast<unsigned long>(postAttempts),
                static_cast<unsigned long>(postSuccesses),
                static_cast<unsigned long>(postFailures));
  Serial.printf("[STATUS] Last HTTP code: %d\n", lastHttpCode);

  uint8_t luxDetected = 0;
  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    if (luxAvailable[i]) luxDetected++;
  }
  Serial.printf("[STATUS] Lux initialized: %u/%u\n", luxDetected, ACTIVE_LUX_SENSORS);
  Serial.printf("[STATUS] DHT configured: %u\n", ACTIVE_DHT_SENSORS);
  Serial.printf("[STATUS] Moisture enabled: %s\n", ENABLE_MOISTURE ? "YES" : "NO");
  Serial.println("[STATUS] ================================");
}

void handleSerialCommands() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) return;

  Serial.printf("[CMD] %s\n", command.c_str());

  if (command == "help" || command == "?") {
    printHelp();
  } else if (command == "status") {
    printStatus();
  } else if (command == "wifi") {
    printWiFiStatus();
  } else if (command == "scan") {
    scanI2CBus();
  } else if (command == "muxscan") {
    scanMuxChannels();
  } else if (command == "sensors") {
    printCachedSensors();
  } else if (command == "read") {
    buildPayload();
  } else if (command == "post") {
    String payload = buildPayload();
    postToMain(payload);
  } else if (command == "initlux") {
    initializeLuxSensors();
  } else if (command == "debug on") {
    debugEnabled = true;
    Serial.println("[DEBUG] Verbose debugging ON");
  } else if (command == "debug off") {
    debugEnabled = false;
    Serial.println("[DEBUG] Verbose debugging OFF");
  } else if (command == "reboot") {
    Serial.println("[SYSTEM] Rebooting...");
    delay(200);
    ESP.restart();
  } else {
    Serial.println("[CMD] Unknown command. Type 'help'.");
  }
}

void printHeartbeat() {
  if (millis() - lastHeartbeatMillis < HEARTBEAT_INTERVAL_MS) return;
  lastHeartbeatMillis = millis();

  Serial.printf("[HEARTBEAT] uptime=%lus wifi=%s",
                static_cast<unsigned long>(millis() / 1000UL),
                wifiStatusName(WiFi.status()));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" rssi=%ddBm", WiFi.RSSI());
  }
  Serial.printf(" seq=%lu HTTP=%lu/%lu/%lu freeHeap=%u\n",
                static_cast<unsigned long>(sequenceNumber),
                static_cast<unsigned long>(postAttempts),
                static_cast<unsigned long>(postSuccesses),
                static_cast<unsigned long>(postFailures),
                ESP.getFreeHeap());
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("====================================================");
  Serial.println(" MushroomFarm ESP32-C3 Node 1 - SERIAL DEBUG BUILD");
  Serial.println("====================================================");
  Serial.printf("Node ID: %s\n", NODE_ID);
  Serial.printf("Target Main SSID: %s\n", MAIN_AP_SSID);
  Serial.printf("Target URL: %s\n", MAIN_DATA_URL);
  Serial.printf("I2C: SDA=GPIO%u, SCL=GPIO%u @ 100kHz\n", I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.println("Sensors: 9x VEML7700 + 3x DHT11");
  Serial.println("Moisture: DISABLED (code retained)");
  Serial.println("Serial Monitor: 115200 baud, New Line ending");
  Serial.println("Type 'help' for commands.");
  Serial.println("====================================================");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  scanI2CBus();
  beginDHTSensors();

  if (ENABLE_MOISTURE) {
    analogReadResolution(12);
    for (uint8_t i = 0; i < CONFIGURED_MOISTURE_SENSORS; i++) {
      pinMode(MOISTURE_PINS[i], INPUT);
    }
  }

  initializeLuxSensors();
  connectToMainWiFi();

  printStatus();
  printHelp();

  // Trigger first automatic sample immediately.
  lastPostMillis = millis() - POST_INTERVAL_MS;
  lastHeartbeatMillis = millis();
}

void loop() {
  handleSerialCommands();
  maintainWiFi();
  printHeartbeat();

  if (millis() - lastPostMillis >= POST_INTERVAL_MS) {
    lastPostMillis = millis();

    String payload = buildPayload();
    if (WiFi.status() == WL_CONNECTED) {
      postToMain(payload);
    } else {
      Serial.println("[HTTP] Packet not sent: Wi-Fi is offline");
    }
  }

  delay(20);
}
