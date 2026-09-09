#include "ControlCore.h"
#include <cmath>
#include <cstring>

namespace farm {
int sensorIndex(const char* id) {
  if (!id) return -1;
  for (size_t i = 0; i < SENSOR_COUNT; ++i)
    if (std::strcmp(id, sensorName(i)) == 0) return static_cast<int>(i);
  return -1;
}
const char* sensorName(size_t i) {
  static const char* names[] = {"sensor1", "sensor2", "sensor3"};
  return i < SENSOR_COUNT ? names[i] : "unknown";
}
const char* relayName(Relay r) {
  static const char* names[] = {"Mist Maker 1", "Mist Maker 2", "Mist Maker 3",
                               "Exhaust Fan", "Sprinkler Pump", "Lights"};
  return r < RelayCount ? names[r] : "Unknown";
}
const char* modeName(Mode m) {
  return m == Mode::Auto ? "auto" : (m == Mode::ManualOn ? "on" : "off");
}
bool validCalendar(int y, int m, int d, int h, int n, int s) {
  if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 ||
      h < 0 || h > 23 || n < 0 || n > 59 || s < 0 || s > 59) return false;
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int limit = days[m - 1];
  if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ++limit;
  return d <= limit;
}
bool lightsScheduled(const Clock& c) {
  return c.valid && (c.hour >= LIGHTS_ON_HOUR || c.hour < LIGHTS_OFF_HOUR);
}
uint64_t windowKey(const Clock& c) {
  const uint32_t window = c.valid ? c.localEpoch / SAMPLE_WINDOW_SECONDS :
      static_cast<uint32_t>(c.uptimeMs / (SAMPLE_WINDOW_SECONDS * 1000ULL));
  return (uint64_t(c.valid) << 63) | (uint64_t(c.generation & 0x7FFFFFFFu) << 32) | window;
}
bool ControlCore::fresh(size_t i, uint32_t now) const {
  return i < SENSOR_COUNT && sensors[i].seen && !sensors[i].timedOut &&
         uint32_t(now - sensors[i].lastSeenMs) < SENSOR_TIMEOUT_MS;
}
bool ControlCore::accept(size_t i, float t, float h, const int32_t* v,
                         size_t count, uint32_t now, const Clock& c) {
  if (i >= SENSOR_COUNT || !std::isfinite(t) || !std::isfinite(h) ||
      t < -40.0f || t > 80.0f || h < 0.0f || h > 100.0f ||
      count > SPECTRO_MAX || (count && !v)) return false;
  Sensor& s = sensors[i];
  s.seen = true;
  s.timedOut = false;
  s.temperature = t;
  s.humidity = h;
  s.lastSeenMs = now;
  s.lastLocalEpoch = c.valid ? c.localEpoch : 0;
  s.spectroCount = static_cast<uint8_t>(count);
  for (size_t k = 0; k < SPECTRO_MAX; ++k) s.spectro[k] = k < count ? v[k] : 0;
  return true;
}
void ControlCore::setPump(bool on, uint32_t now) {
  if (on && !relays[Sprinkler].on) pumpStartedMs_ = now;
  relays[Sprinkler].on = on;
}
bool ControlCore::setMode(Relay r, Mode mode, uint32_t now) {
  if (r >= RelayCount || !hardwareReady) return false;
  // Repeating ON never extends an already-running pump's time limit.
  relays[r].mode = mode;
  if (r == Sprinkler && mode != Mode::Auto) setPump(mode == Mode::ManualOn, now);
  return true;
}
void ControlCore::resetModes() {
  for (auto& r : relays) r.mode = Mode::Auto;
}
void ControlCore::evaluate(uint32_t now, const Clock& c) {
  for (auto& sensor : sensors)
    if (sensor.seen && uint32_t(now - sensor.lastSeenMs) >= SENSOR_TIMEOUT_MS) sensor.timedOut = true;
  if (!hardwareReady) {
    for (auto& r : relays) r.on = false;
    c3PowerCut = false;
    return;
  }
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    auto& r = relays[i];
    if (r.mode != Mode::Auto) r.on = r.mode == Mode::ManualOn;
    else if (!fresh(i, now)) r.on = false;
    else if (sensors[i].humidity < HUMIDITY_ON_BELOW) r.on = true;
    else if (sensors[i].humidity >= HUMIDITY_OFF_AT) r.on = false;
  }
  auto& fan = relays[Exhaust];
  bool outside = false;
  for (size_t i = 0; i < SENSOR_COUNT; ++i)
    if (fresh(i, now) && (sensors[i].temperature > EXHAUST_HIGH ||
        (EXHAUST_ON_LOW_TEMPERATURE && sensors[i].temperature < EXHAUST_LOW))) outside = true;
  fan.on = fan.mode == Mode::Auto ? outside : fan.mode == Mode::ManualOn;
  auto& lights = relays[Lights];
  lights.on = lights.mode == Mode::Auto ? lightsScheduled(c) : lights.mode == Mode::ManualOn;

  auto& pump = relays[Sprinkler];
  if (pump.on && uint32_t(now - pumpStartedMs_) >= SPRINKLER_DURATION_MS) {
    setPump(false, now);
    if (pump.mode == Mode::ManualOn) pump.mode = Mode::ManualOff;
  }
  if (pump.mode == Mode::ManualOff) setPump(false, now);
  else if (pump.mode == Mode::Auto) {
    if (!c.valid) setPump(false, now);
    else if (!pump.on && c.hour == SPRINKLER_HOUR && c.minute == SPRINKLER_MINUTE &&
             c.dayKey > lastSprinklerDay) {
      lastSprinklerDay = c.dayKey;
      sprinklerDayChanged = true;
      setPump(true, now);
    }
  }
}
void ControlCore::monitorNodes(uint32_t now) {
  if (!hardwareReady || !ENABLE_C3_POWER_RECOVERY) return;
  if (c3PowerCut) {
    if (uint32_t(now - powerCutStartedMs_) >= C3_POWER_OFF_MS) {
      c3PowerCut = false;
      powerRestoredMs_ = now;
      hasPowerCycled_ = true;
    }
    return;
  }
  if ((!hasPowerCycled_ && now < C3_BOOT_GRACE_MS) ||
      (hasPowerCycled_ && uint32_t(now - powerRestoredMs_) < C3_RECOVERY_COOLDOWN_MS)) return;
  for (size_t i = 0; i < SENSOR_COUNT; ++i) {
    if (!fresh(i, now)) {
      c3PowerCut = true;
      powerCutStartedMs_ = now;
      return;
    }
  }
}
}
