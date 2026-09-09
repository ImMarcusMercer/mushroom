#include "Firmware.h"
#include "ControlCore.h"
#include "LogWriter.h"
#include "NetworkConfig.h"
#include "Dashboard.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <SD.h>
#include <Preferences.h>
#include <Adafruit_MCP23X17.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#if MUSHROOM_ENABLE_TFT
#include <TFT_eSPI.h>
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "This controller requires a classic ESP32 (VSPI/HSPI and GPIO33), not an ESP32-C3."
#endif
#if ARDUINOJSON_VERSION_MAJOR != 7
#error "Install ArduinoJson 7.x using the Arduino Library Manager."
#endif

namespace {
using namespace farm;
ControlCore core;
History history;
Clock clockNow;
WebServer server(80);
SPIClass sdBus(HSPI);
RTC_DS3231 rtc;
Adafruit_MCP23X17 mcp;
Preferences preferences;
bool preferencesReady = false, rtcAvailable = false, rtcTrusted = false;
bool flushRequested = false;
bool loggingTimerStarted = false;
uint32_t lastClockMs = 0, lastHardwareCheckMs = 0, lastSdAttemptMs = 0;
uint32_t lastAutoLogMs = 0, lastLogAttemptMs = 0;
uint8_t writtenRelayMask = 0xFF;
constexpr uint8_t USED_RELAY_MASK = 0xBF; // GPA0..5 plus GPA7. GPA6 is unused.
#if MUSHROOM_ENABLE_TFT
TFT_eSPI tft;
bool displayReady = false;
#endif

class SdStorage : public LogStorage {
public:
  bool available = false;
  bool ready() const override { return available; }
  bool mount() {
    SD.end();
    available = SD.begin(SD_CS, sdBus, 4000000) && SD.cardType() != CARD_NONE;
    if (available && !SD.exists("/logs") && !SD.mkdir("/logs")) available = false;
    return available;
  }
  bool create(const char* path) override {
    if (!available || SD.exists(path)) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) { available = false; return false; }
    f.close();
    return true;
  }
  bool append(const char* path, const char* data, size_t length) override {
    if (!available) return false;
    File f = SD.open(path, FILE_APPEND);
    if (!f) { available = false; return false; }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(data), length);
    f.flush();
    f.close();
    if (written != length) available = false;
    return written == length;
  }
  size_t read(const char* path, size_t offset, uint8_t* buffer, size_t length) override {
    if (!available) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f || !f.seek(offset)) { f.close(); available = false; return 0; }
    const size_t n = f.read(buffer, length);
    f.close();
    if (n != length) available = false;
    return n;
  }
  int64_t fileSize(const char* path) override {
    File f = SD.open(path, FILE_READ);
    if (!f) { available = false; return -1; }
    const int64_t n = f.size();
    f.close();
    return n;
  }
  bool rename(const char* from, const char* to) override {
    return available && !SD.exists(to) && SD.rename(from, to);
  }
  void remove(const char* path) override {
    if (available && SD.exists(path)) SD.remove(path);
  }
} disk;
LogWriter logWriter(history, disk);

bool probeI2c(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}
void readClock(uint32_t now, bool force = false) {
  clockNow.uptimeMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
  if (!force && uint32_t(now - lastClockMs) < 250) return;
  lastClockMs = now;
  const bool previouslyValid = clockNow.valid;
  clockNow.valid = false;
  if (rtcAvailable && probeI2c(0x68)) {
    const DateTime t = rtc.now();
    if (rtcTrusted && !rtc.lostPower() && validCalendar(t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second())) {
      clockNow.valid = true;
      clockNow.localEpoch = t.unixtime();
      clockNow.hour = t.hour();
      clockNow.minute = t.minute();
      clockNow.dayKey = uint32_t(t.year()) * 10000 + t.month() * 100 + t.day();
    }
  } else rtcAvailable = false;
  if (previouslyValid != clockNow.valid) ++clockNow.generation;
}
String timeText() {
  if (!clockNow.valid) return "Clock not synchronized";
  Record r;
  r.localEpoch = clockNow.localEpoch;
  char date[24], time[32];
  formatRecordTime(r, date, sizeof(date), time, sizeof(time));
  return String(date) + " " + time;
}
void initializeRelays() {
  if (!probeI2c(MCP_ADDRESS) || !mcp.begin_I2C(MCP_ADDRESS, &Wire)) {
    core.hardwareReady = false;
    return;
  }
  // Preload inactive HIGH latches before changing any relay pin to output.
  mcp.writeGPIOAB(0xFFFF);
  for (uint8_t pin : RELAY_PINS) mcp.pinMode(pin, OUTPUT);
  mcp.pinMode(C3_POWER_PIN, OUTPUT);
  writtenRelayMask = 0xFF;
  core.hardwareReady = (mcp.readGPIOA() & USED_RELAY_MASK) == USED_RELAY_MASK;
  core.resetModes();
  for (auto& r : core.relays) r.on = false;
  core.c3PowerCut = false;
}
void applyRelays() {
  if (!core.hardwareReady) return;
  uint8_t mask = 0xFF;
  for (size_t i = 0; i < RelayCount; ++i)
    if (core.relays[i].on) mask &= static_cast<uint8_t>(~(1u << RELAY_PINS[i]));
  if (core.c3PowerCut) mask &= static_cast<uint8_t>(~(1u << C3_POWER_PIN));
  if (mask == writtenRelayMask) return;
  if (!probeI2c(MCP_ADDRESS)) { core.hardwareReady = false; return; }
  mcp.writeGPIOA(mask);
  if ((mcp.readGPIOA() & USED_RELAY_MASK) != (mask & USED_RELAY_MASK)) {
    core.hardwareReady = false;
    Serial.println("Relay expander verification failed; output state is unknown.");
    return;
  }
  writtenRelayMask = mask;
  Serial.printf("Relay output mask: 0x%02X\n", mask);
}
void updateControls(uint32_t now) {
  core.evaluate(now, clockNow);
  if (core.sprinklerDayChanged) {
    // Record the scheduled run BEFORE energizing the output, preventing a reset
    // during the scheduled minute from starting it for a second time.
    if (!preferencesReady || preferences.putULong("pumpDay", core.lastSprinklerDay) != sizeof(uint32_t)) {
      core.setMode(Sprinkler, Mode::ManualOff, now);
      Serial.println("Scheduled sprinkler skipped: cannot save the run date.");
    }
    core.sprinklerDayChanged = false;
  }
  core.monitorNodes(now);
  applyRelays();
  if (!core.hardwareReady || !core.relays[Lights].on)
    for (auto& sensor : core.sensors) sensor.spectroCount = 0;
}
bool authorized() {
  if (!WEB_PASSWORD[0] || server.authenticate(WEB_USER, WEB_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}
bool parseBody(JsonDocument& doc) {
  const String body = server.arg("plain");
  if (body.length() == 0 || body.length() > MAX_JSON_BYTES) {
    server.send(413, "text/plain", "JSON body must contain 1 to 768 bytes"); return false;
  }
  const auto error = deserializeJson(doc, body, DeserializationOption::NestingLimit(4));
  if (error || !doc.is<JsonObject>()) {
    server.send(400, "text/plain", "Expected a valid JSON object"); return false;
  }
  return true;
}
void handleData() {
  if (SENSOR_API_KEY[0] && server.header("X-Sensor-Key") != SENSOR_API_KEY) {
    server.send(401, "text/plain", "Invalid sensor key"); return;
  }
  JsonDocument doc;
  if (!parseBody(doc)) return;
  if (!doc["id"].is<const char*>() || !doc["temp"].is<float>() || !doc["hum"].is<float>()) {
    server.send(400, "text/plain", "id, numeric temp and numeric hum are required"); return;
  }
  const int index = sensorIndex(doc["id"].as<const char*>());
  if (index < 0) { server.send(400, "text/plain", "Expected sensor1, sensor2 or sensor3"); return; }
  int32_t values[SPECTRO_MAX] = {};
  size_t count = 0;
  if (!doc["spectro"].isNull()) {
    if (!doc["spectro"].is<JsonArray>()) { server.send(400, "text/plain", "spectro must be an array"); return; }
    JsonArray array = doc["spectro"].as<JsonArray>();
    count = array.size();
    if ((count != 0 && count != 3 && count != 6) || (index == 2 && count > 3)) {
      server.send(400, "text/plain", "spectro must contain 0/3/6 integers; sensor3 supports 0/3"); return;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!array[i].is<int32_t>()) { server.send(400, "text/plain", "spectro values must be signed 32-bit integers"); return; }
      values[i] = array[i].as<int32_t>();
    }
  }
  const uint32_t now = millis();
  readClock(now);
  if (!core.accept(index, doc["temp"].as<float>(), doc["hum"].as<float>(), values, count, now, clockNow)) {
    server.send(400, "text/plain", "Temperature must be -40 to 80 C and humidity 0 to 100 percent"); return;
  }
  updateControls(now);
  if (!core.relays[Lights].on || !core.hardwareReady) core.sensors[index].spectroCount = 0;
  history.collect(index, core, clockNow);
  if (!loggingTimerStarted) { loggingTimerStarted = true; lastAutoLogMs = now; }
  // A full history buffer never invalidates a fresh measurement for control.
  // The cumulative omission count remains visible in /status and the dashboard.
  server.send(200, "text/plain", "OK");
}
void handleStatus() {
  if (!authorized()) return;
  const uint32_t now = millis();
  readClock(now);
  JsonDocument doc;
  doc["version"] = VERSION;
  doc["rtc_available"] = rtcAvailable;
  doc["clock_valid"] = clockNow.valid;
  doc["time"] = timeText();
  doc["sd_available"] = disk.available;
  doc["relays_available"] = core.hardwareReady;
  doc["lights_on"] = core.hardwareReady && core.relays[Lights].on;
  doc["c3_power_cut"] = core.c3PowerCut;
  doc["history_count"] = history.size;
  doc["history_capacity"] = HISTORY_CAPACITY;
  doc["frozen_count"] = history.frozen;
  doc["dropped_history"] = history.dropped;
  doc["logging"] = logWriter.busy();
  doc["log_error"] = logWriter.error;
  doc["last_log"] = logWriter.lastCompleted;
  doc["completed_exports"] = logWriter.completed;
  doc["free_heap"] = ESP.getFreeHeap();
  auto sensors = doc["sensors"].to<JsonArray>();
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    const auto& s = core.sensors[i];
    auto entry = sensors.add<JsonObject>();
    entry["id"] = sensorName(i);
    entry["seen"] = s.seen;
    entry["fresh"] = core.fresh(i, now);
    entry["temp"] = s.temperature;
    entry["hum"] = s.humidity;
    entry["age_ms"] = s.seen ? uint32_t(now - s.lastSeenMs) : 0;
  }
  auto relays = doc["relays"].to<JsonArray>();
  for (size_t i = 0; i < RelayCount; ++i) {
    auto entry = relays.add<JsonObject>();
    entry["id"] = i == Lights ? 7 : i + 1; // preserve the original HTTP numbering
    entry["name"] = relayName(static_cast<Relay>(i));
    entry["on"] = core.relays[i].on;
    entry["mode"] = modeName(core.relays[i].mode);
  }
  auto treatments = doc["treatments"].to<JsonArray>();
  for (size_t t = 0; t < 5; ++t) {
    auto entry = treatments.add<JsonObject>();
    entry["treatment"] = t + 1;
    auto values = entry["values"].to<JsonArray>();
    const size_t s = t / 2, start = (t % 2) * 3;
    if (core.hardwareReady && core.relays[Lights].on && core.fresh(s, now) && core.sensors[s].spectroCount >= start + 3)
      for (size_t k = 0; k < 3; ++k) values.add(core.sensors[s].spectro[start + k]);
  }
  if (doc.overflowed()) { server.send(503, "text/plain", "Not enough memory for status"); return; }
  String json;
  serializeJson(doc, json);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}
bool setTime(int y, int m, int d, int h, int n, int s) {
  if (!rtcAvailable || !validCalendar(y, m, d, h, n, s)) return false;
  const DateTime desired(y, m, d, h, n, s);
  rtc.adjust(desired);
  if (!probeI2c(0x68)) return false;
  const uint32_t actual = rtc.now().unixtime();
  if (actual < desired.unixtime() || actual - desired.unixtime() > 2 || rtc.lostPower()) return false;
  rtcTrusted = true;
  ++clockNow.generation;
  readClock(millis(), true);
  updateControls(millis());
  return true;
}
void handleTimeSync() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const char* keys[] = {"year", "month", "day", "hour", "minute", "second"};
  for (const char* key : keys) {
    if (!doc[key].is<int>()) { server.send(400, "text/plain", "Six integer date/time fields are required"); return; }
  }
  if (!validCalendar(doc["year"], doc["month"], doc["day"], doc["hour"], doc["minute"], doc["second"])) {
    server.send(400, "text/plain", "Invalid calendar date or time"); return;
  }
  if (!setTime(doc["year"], doc["month"], doc["day"], doc["hour"], doc["minute"], doc["second"])) {
    server.send(503, "text/plain", "RTC unavailable or time write could not be verified"); return;
  }
  server.send(200, "text/plain", "Manila time synchronized");
}
void handleRelay() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!parseBody(doc)) return;
  if (!doc["relay"].is<int>() || !doc["action"].is<const char*>()) {
    server.send(400, "text/plain", "Integer relay and action are required"); return;
  }
  const int number = doc["relay"];
  if (number < 1 || number > 7 || number == 6) {
    server.send(400, "text/plain", "Relay must be 1, 2, 3, 4, 5 or 7"); return;
  }
  const char* action = doc["action"];
  Mode mode;
  if (std::strcmp(action, "on") == 0) mode = Mode::ManualOn;
  else if (std::strcmp(action, "off") == 0) mode = Mode::ManualOff;
  else if (std::strcmp(action, "auto") == 0) mode = Mode::Auto;
  else { server.send(400, "text/plain", "action must be on, off or auto"); return; }
  const Relay relay = number == 7 ? Lights : static_cast<Relay>(number - 1);
  if (!core.setMode(relay, mode, millis())) { server.send(503, "text/plain", "Relay expander unavailable"); return; }
  readClock(millis());
  updateControls(millis());
  if (relay == Lights && !core.relays[Lights].on)
    for (auto& sensor : core.sensors) sensor.spectroCount = 0;
  if (!core.hardwareReady) { server.send(503, "text/plain", "Relay output could not be verified"); return; }
  server.send(200, "text/plain", "OK");
}
void configureWeb() {
  const char* headers[] = {"X-Sensor-Key"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, []() {
    if (!authorized()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
  });
  server.on("/data", HTTP_POST, handleData);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/sync-time", HTTP_POST, handleTimeSync);
  server.on("/relay-control", HTTP_POST, handleRelay);
  server.on("/reset-overrides", HTTP_POST, []() {
    if (!authorized()) return;
    if (!core.hardwareReady) { server.send(503, "text/plain", "Relay expander unavailable"); return; }
    core.resetModes();
    updateControls(millis());
    server.send(200, "text/plain", "Automatic control restored");
  });
  server.on("/log-now", HTTP_POST, []() {
    if (!authorized()) return;
    if (logWriter.busy()) { server.send(409, "text/plain", "An export is already in progress"); return; }
    if (!history.size) { server.send(200, "text/plain", "No pending readings"); return; }
    flushRequested = true;
    server.send(202, "text/plain", "Export requested; check logging status for completion");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found or unsupported method"); });
  server.begin();
}

void updateDisplay(uint32_t now) {
#if MUSHROOM_ENABLE_TFT
  static uint32_t lastDisplayMs = 0, buttonChangedMs = 0;
  static bool rawPrevious = HIGH, stableButton = HIGH, spectroMode = false;
  const bool raw = digitalRead(BUTTON_PIN);
  bool changedMode = false;
  if (raw != rawPrevious) { buttonChangedMs = now; rawPrevious = raw; }
  if (uint32_t(now - buttonChangedMs) >= 50 && raw != stableButton) {
    stableButton = raw;
    if (!stableButton) { spectroMode = !spectroMode; changedMode = true; }
  }
  if (!displayReady || (!changedMode && uint32_t(now - lastDisplayMs) < 1000)) return;
  lastDisplayMs = now;
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.fillRect(8, 34, 464, 46, TFT_BLACK);
  tft.setCursor(8, 34);
  tft.print(timeText());
  tft.setCursor(8, 58);
  tft.setTextSize(1);
  tft.printf("SD: %s | Buffer: %u/%u | Relays: %s", disk.available ? "OK" : "OFFLINE",
      static_cast<unsigned>(history.size), static_cast<unsigned>(HISTORY_CAPACITY), core.hardwareReady ? "OK" : "UNKNOWN");
  tft.fillRect(8, 90, 464, 188, TFT_BLACK);
  if (!spectroMode) {
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 92); tft.print("Sensor     Temp      Humidity");
    for (size_t i = 0; i < SENSOR_COUNT; ++i) {
      tft.setCursor(10, 132 + i * 40);
      tft.setTextColor(core.fresh(i, now) ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
      tft.printf("%s", sensorName(i));
      tft.setCursor(145, 132 + i * 40);
      if (core.fresh(i, now)) tft.printf("%5.1f C    %5.1f%%", core.sensors[i].temperature, core.sensors[i].humidity);
      else tft.print(core.sensors[i].seen ? "Stale" : "Waiting");
    }
  } else if (!core.hardwareReady || !core.relays[Lights].on) {
    tft.setTextSize(2); tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(24, 145); tft.print("Spectro collection disabled");
    tft.setCursor(24, 180); tft.print("Light output is OFF");
  } else {
    tft.setTextSize(1);
    for (size_t t = 0; t < 5; ++t) {
      const size_t s = t / 2, offset = (t % 2) * 3;
      const int x = 10 + t * 93;
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(x, 100); tft.printf("Treatment%u", static_cast<unsigned>(t + 1));
      tft.setTextColor(t == 4 ? TFT_RED : t == 3 ? TFT_CYAN : TFT_GREEN, TFT_BLACK);
      for (unsigned k = 0; k < 3; ++k) {
        tft.setCursor(x, 140 + k * 30);
        if (core.fresh(s, now) && core.sensors[s].spectroCount >= offset + 3)
          tft.printf("%ld", static_cast<long>(core.sensors[s].spectro[offset + k]));
        else tft.print("--");
      }
    }
  }
  tft.fillRect(0, 285, 480, 35, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY); tft.setTextSize(1);
  tft.setCursor(12, 298);
  tft.print(spectroMode ? "Press button for temperature and humidity" : "Press button for spectrometer readings");
#else
  (void)now;
#endif
}
void listFiles() {
  if (!disk.available) { Serial.println("SD unavailable"); return; }
  File dir = SD.open("/logs");
  if (!dir) return;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    Serial.printf("%s (%u bytes)\n", f.name(), static_cast<unsigned>(f.size()));
    f.close();
    updateControls(millis());
  }
  dir.close();
}
void handleSerial() {
  static char command[100];
  static size_t used = 0;
  static bool overflow = false;
  unsigned budget = 64;
  while (Serial.available() && budget--) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (used + 1 < sizeof(command)) command[used++] = ch;
      else overflow = true;
      continue;
    }
    command[used] = '\0'; used = 0;
    if (overflow) { overflow = false; Serial.println("Command too long"); continue; }
    if (!std::strcmp(command, "LOG_NOW")) flushRequested = true;
    else if (!std::strcmp(command, "BUFFER_STATUS"))
      Serial.printf("Pending=%u, frozen=%u, capacity=%u, omitted=%lu, heap=%u, logging=%s, error=%s\n",
          static_cast<unsigned>(history.size), static_cast<unsigned>(history.frozen), static_cast<unsigned>(HISTORY_CAPACITY),
          static_cast<unsigned long>(history.dropped), static_cast<unsigned>(ESP.getFreeHeap()), logWriter.busy() ? "yes" : "no", logWriter.error);
    else if (!std::strcmp(command, "RESET_OVERRIDES")) { core.resetModes(); updateControls(millis()); Serial.println("Automatic modes restored"); }
    else if (!std::strcmp(command, "LIST_FILES")) listFiles();
    else if (!std::strncmp(command, "SET_TIME ", 9)) {
      int y, m, d, h, n, s;
      char extra;
      if (std::sscanf(command + 9, "%d %d %d %d %d %d %c", &y, &m, &d, &h, &n, &s, &extra) != 6 || !setTime(y, m, d, h, n, s))
        Serial.println("Invalid date/time or RTC unavailable");
      else Serial.println(timeText());
    } else Serial.println("LOG_NOW | BUFFER_STATUS | LIST_FILES | RESET_OVERRIDES | SET_TIME YYYY MM DD HH MM SS | HELP");
  }
}
}

void firmwareSetup() {
  Serial.begin(115200);
  Serial.printf("\nMushroomFarm Monitor %s\n", farm::VERSION);
  pinMode(farm::SD_CS, OUTPUT); digitalWrite(farm::SD_CS, HIGH);
  pinMode(5, OUTPUT); digitalWrite(5, HIGH); // existing TFT CS
  pinMode(farm::BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(farm::I2C_SDA, farm::I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  initializeRelays();
  preferencesReady = preferences.begin("mushroom", false);
  if (preferencesReady) core.lastSprinklerDay = preferences.getULong("pumpDay", 0);
  rtcAvailable = rtc.begin(&Wire);
  rtcTrusted = rtcAvailable && !rtc.lostPower();
  readClock(millis(), true);
  sdBus.begin(farm::SD_SCK, farm::SD_MISO, farm::SD_MOSI, farm::SD_CS);
  disk.mount();
  logWriter.setBootId(esp_random(), esp_random());
#if MUSHROOM_ENABLE_TFT
  // TFT_eSPI's selected setup applies to both the sketch and library sources.
  // No driver or missing DC/RST pin is guessed here.
  bool pinConfigMatches = true;
#if !defined(TFT_MOSI) || !defined(TFT_MISO) || !defined(TFT_SCLK) || !defined(TFT_CS)
  pinConfigMatches = false;
#else
  pinConfigMatches = TFT_MOSI == 23 && TFT_MISO == 19 && TFT_SCLK == 18 && TFT_CS == 5;
#endif
#ifdef USE_HSPI_PORT
  pinConfigMatches = false;
#endif
  if (pinConfigMatches) {
    tft.init(); tft.setRotation(3);
    displayReady = tft.width() == 480 && tft.height() == 320;
    if (displayReady) {
      tft.fillScreen(TFT_BLACK); tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(8, 8); tft.printf("MushroomFarm v%s", farm::VERSION);
    }
  }
  if (!displayReady) Serial.println("TFT setup does not match the 480x320 VSPI display. See docs/SETUP.md. Web control continues.");
#endif
  WiFi.mode(WIFI_AP);
  const IPAddress ip(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(ip, ip, subnet);
  if (!WiFi.softAP(farm::AP_SSID, farm::AP_PASSWORD, 1, 0, 8)) Serial.println("Wi-Fi AP failed to start");
  Serial.print("Dashboard: http://"); Serial.println(WiFi.softAPIP());
  configureWeb();
  lastAutoLogMs = millis();
  lastSdAttemptMs = millis();
  updateControls(millis());
}
void firmwareLoop() {
  uint32_t now = millis();
  readClock(now);
  updateControls(now);
  server.handleClient();
  handleSerial();
  now = millis();
  if (uint32_t(now - lastHardwareCheckMs) >= 5000) {
    lastHardwareCheckMs = now;
    if (!core.hardwareReady) initializeRelays();
    else if (!probeI2c(farm::MCP_ADDRESS) ||
             (mcp.readGPIOA() & USED_RELAY_MASK) != (writtenRelayMask & USED_RELAY_MASK)) core.hardwareReady = false;
    if (!rtcAvailable) { rtcAvailable = rtc.begin(&Wire); rtcTrusted = rtcAvailable && !rtc.lostPower(); }
  }
  if (!disk.available && !logWriter.busy() && uint32_t(now - lastSdAttemptMs) >= farm::LOG_RETRY_MS) {
    lastSdAttemptMs = now;
    disk.mount();
  }
  if (loggingTimerStarted && uint32_t(now - lastAutoLogMs) >= farm::LOG_INTERVAL_MS) {
    lastAutoLogMs = now;
    if (history.size) flushRequested = true;
  }
  if (!logWriter.busy() && (flushRequested || logWriter.failed() || history.frozen) &&
      (flushRequested || uint32_t(now - lastLogAttemptMs) >= farm::LOG_RETRY_MS)) {
    flushRequested = false;
    lastLogAttemptMs = now;
    if (history.size && !logWriter.start()) {
      // Keep retrying even when the first attempt happened with no SD mounted.
      history.freeze();
      Serial.println(logWriter.error);
    }
  }
  logWriter.step();
  updateDisplay(now);
  // Recheck deadlines after HTTP, display and SD operations. No deliberate
  // multi-second delays occur in this loop; each driver call can still block.
  readClock(millis());
  updateControls(millis());
  delay(2);
}
