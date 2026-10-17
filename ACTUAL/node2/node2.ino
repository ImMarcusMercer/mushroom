/*
 * MushroomFarm Sensor Node 2
 * Target: ESP32-C3 SuperMini / ESP32-C3 Mini development board
 * Node ID sent to main: sensor2
 *
 * CURRENT BENCH TEST:
 *   - 9 x VEML7700 lux sensors through 2 x TCA9548A multiplexers
 *     * Lux 1..8 -> TCA9548A #1 at 0x70, channels 0..7
 *     * Lux 9    -> TCA9548A #2 at 0x71, channel 0
 *   - 3 x DHT11 temperature + humidity sensors
 *     * DHT11 #1 -> GPIO6
 *     * DHT11 #2 -> GPIO0
 *     * DHT11 #3 -> GPIO1
 *   - Moisture sensing code is RETAINED but DISABLED for this test
 *
 * FUTURE TARGET PER NODE:
 *   - 9 x VEML7700 lux sensors
 *   - 3 x DHT11 sensors (current test already uses this)
 *   - 3 x analog moisture sensors (pins to be finalized when moisture is re-enabled)
 *
 * The node joins the ZY-ESP32 main controller's Wi-Fi AP and POSTs JSON to:
 *   http://192.168.4.1/data
 *
 * No simulated sensor readings are generated. Failed sensor reads are sent as
 * JSON null values. Moisture is explicitly sent as an empty array while the
 * feature is disabled.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <DHT.h>

const char* NODE_ID = "sensor2";
const char* MAIN_AP_SSID = "MushrooMan";
const char* MAIN_AP_PASSWORD = "RABE2026qt";
const char* MAIN_DATA_URL = "http://192.168.4.1/data";

const uint8_t I2C_SDA_PIN = 4;
const uint8_t I2C_SCL_PIN = 5;

const uint8_t DHT_PINS[3] = {6, 0, 1};
#define DHT_TYPE DHT11
DHT dht1(DHT_PINS[0], DHT_TYPE);
DHT dht2(DHT_PINS[1], DHT_TYPE);
DHT dht3(DHT_PINS[2], DHT_TYPE);
DHT* dhtSensors[3] = {&dht1, &dht2, &dht3};

const uint8_t MOISTURE_PINS[3] = {2, 3, 7};
const bool ENABLE_MOISTURE = false;

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

const LuxRoute LUX_ROUTES[MAX_LUX_SENSORS] = {
  {0x70, 0}, {0x70, 1}, {0x70, 2}, {0x70, 3},
  {0x70, 4}, {0x70, 5}, {0x70, 6}, {0x70, 7},
  {0x71, 0}
};

Adafruit_VEML7700 veml;
bool luxAvailable[MAX_LUX_SENSORS] = {false};

const unsigned long POST_INTERVAL_MS = 5000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000UL;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;

unsigned long lastPostMillis = 0;
unsigned long lastWifiAttemptMillis = 0;
uint32_t sequenceNumber = 0;

bool tcaSelect(uint8_t address, uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(address);
  Wire.write(1U << channel);
  return Wire.endTransmission() == 0;
}

void tcaDisableAll(uint8_t address) {
  Wire.beginTransmission(address);
  Wire.write(0x00);
  Wire.endTransmission();
}

void initializeLuxSensors() {
  Serial.println("Initializing VEML7700 sensors through TCA9548A...");
  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) {
    const LuxRoute& route = LUX_ROUTES[i];
    if (!tcaSelect(route.muxAddress, route.channel)) {
      Serial.printf("Lux %u: TCA 0x%02X channel %u unavailable\n", i + 1, route.muxAddress, route.channel);
      luxAvailable[i] = false;
      continue;
    }
    delay(5);
    if (!veml.begin()) {
      Serial.printf("Lux %u: VEML7700 not detected\n", i + 1);
      luxAvailable[i] = false;
      continue;
    }
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    luxAvailable[i] = true;
    Serial.printf("Lux %u: OK (TCA 0x%02X ch %u)\n", i + 1, route.muxAddress, route.channel);
  }
  tcaDisableAll(0x70);
  tcaDisableAll(0x71);
}

bool readLuxSensor(uint8_t index, float& luxOut) {
  if (index >= ACTIVE_LUX_SENSORS || !luxAvailable[index]) return false;
  const LuxRoute& route = LUX_ROUTES[index];
  if (!tcaSelect(route.muxAddress, route.channel)) {
    luxAvailable[index] = false;
    return false;
  }
  delay(3);
  luxOut = veml.readLux();
  return !isnan(luxOut);
}

void beginDHTSensors() {
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    dhtSensors[i]->begin();
    delay(50);
  }
}

void readAllDHT11(float tempC[], float humidity[], bool valid[]) {
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    humidity[i] = dhtSensors[i]->readHumidity();
    tempC[i] = dhtSensors[i]->readTemperature();
    valid[i] = !(isnan(tempC[i]) || isnan(humidity[i]));
    if (!valid[i]) {
      Serial.printf("DHT11 #%u read failed\n", i + 1);
      tempC[i] = 0.0f;
      humidity[i] = 0.0f;
    }
  }
}

int readMoistureRaw(uint8_t pin) {
  const uint8_t samples = 12;
  uint32_t total = 0;
  for (uint8_t i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return static_cast<int>(total / samples);
}

bool connectToMainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("Connecting to main AP: ");
  Serial.println(MAIN_AP_SSID);
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
    Serial.print("Connected. Node IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi connection timed out");
  return false;
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMillis < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiAttemptMillis = millis();
  WiFi.disconnect(true);
  delay(50);
  connectToMainWiFi();
}

String buildPayload() {
  float luxValues[MAX_LUX_SENSORS] = {0};
  bool luxValid[MAX_LUX_SENSORS] = {false};
  for (uint8_t i = 0; i < ACTIVE_LUX_SENSORS; i++) luxValid[i] = readLuxSensor(i, luxValues[i]);
  tcaDisableAll(0x70);
  tcaDisableAll(0x71);

  float dhtTemp[MAX_DHT_SENSORS] = {0};
  float dhtHum[MAX_DHT_SENSORS] = {0};
  bool dhtValid[MAX_DHT_SENSORS] = {false};
  readAllDHT11(dhtTemp, dhtHum, dhtValid);

  int moistureValues[MAX_MOISTURE_SENSORS] = {0};
  if (ENABLE_MOISTURE) {
    for (uint8_t i = 0; i < CONFIGURED_MOISTURE_SENSORS; i++) moistureValues[i] = readMoistureRaw(MOISTURE_PINS[i]);
  }

  float tempSum = 0.0f, humSum = 0.0f;
  uint8_t dhtValidCount = 0;
  for (uint8_t i = 0; i < ACTIVE_DHT_SENSORS; i++) {
    if (dhtValid[i]) { tempSum += dhtTemp[i]; humSum += dhtHum[i]; dhtValidCount++; }
  }
  bool hasAverage = dhtValidCount > 0;
  float avgTemp = hasAverage ? tempSum / dhtValidCount : 0.0f;
  float avgHum = hasAverage ? humSum / dhtValidCount : 0.0f;

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

  Serial.println("Sensor payload:");
  Serial.println(payload);
  return payload;
}

bool postToMain(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  if (!http.begin(MAIN_DATA_URL)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int responseCode = http.POST(payload);
  if (responseCode > 0) {
    String response = http.getString();
    Serial.printf("Main response: HTTP %d - %s\n", responseCode, response.c_str());
  } else {
    Serial.printf("POST failed: %s\n", http.errorToString(responseCode).c_str());
  }
  http.end();
  return responseCode == 200;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("========================================");
  Serial.print("MushroomFarm ESP32-C3 Node ");
  Serial.println(2);
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);
  Serial.println("Test sensors: 9x VEML7700 + 3x DHT11");
  Serial.println("Moisture: DISABLED (code retained)");
  Serial.println("========================================");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  beginDHTSensors();
  if (ENABLE_MOISTURE) {
    analogReadResolution(12);
    for (uint8_t i = 0; i < CONFIGURED_MOISTURE_SENSORS; i++) pinMode(MOISTURE_PINS[i], INPUT);
  }
  initializeLuxSensors();
  connectToMainWiFi();
  lastPostMillis = millis() - POST_INTERVAL_MS;
}

void loop() {
  maintainWiFi();
  if (millis() - lastPostMillis >= POST_INTERVAL_MS) {
    lastPostMillis = millis();
    String payload = buildPayload();
    if (WiFi.status() == WL_CONNECTED) postToMain(payload);
    else Serial.println("Packet not sent: Wi-Fi is offline");
  }
  delay(20);
}
