#pragma once
#include "Config.h"

namespace farm {
enum class Mode : uint8_t { Auto, ManualOn, ManualOff };
enum Relay : uint8_t { Mist1, Mist2, Mist3, Exhaust, Sprinkler, Lights, RelayCount };
struct Clock {
  bool valid = false;
  uint32_t localEpoch = 0;  // RTC stores Manila civil time, not UTC.
  uint64_t uptimeMs = 0;
  uint32_t generation = 0; // Changes after a time correction; separates windows.
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint32_t dayKey = 0;     // YYYYMMDD; also used to persist the last pump run.
};
struct Sensor {
  bool seen = false;
  bool timedOut = false;  // Latched until a valid packet arrives; prevents revival after a long millis wrap.
  float temperature = 0;
  float humidity = 0;
  uint32_t lastSeenMs = 0;
  uint32_t lastLocalEpoch = 0;
  uint8_t spectroCount = 0;
  int32_t spectro[SPECTRO_MAX] = {};
};
struct RelayState {
  Mode mode = Mode::Auto;
  bool on = false;        // Commanded output; there is no physical relay feedback.
};
class ControlCore {
public:
  Sensor sensors[SENSOR_COUNT] = {};
  RelayState relays[RelayCount] = {};
  bool hardwareReady = false;
  bool c3PowerCut = false;
  uint32_t lastSprinklerDay = 0;
  bool sprinklerDayChanged = false;
  bool fresh(size_t sensor, uint32_t now) const;
  bool accept(size_t sensor, float temp, float hum, const int32_t* values,
              size_t count, uint32_t now, const Clock& clock);
  bool setMode(Relay relay, Mode mode, uint32_t now);
  void resetModes();
  void evaluate(uint32_t now, const Clock& clock);
  void monitorNodes(uint32_t now);
private:
  uint32_t pumpStartedMs_ = 0;
  uint32_t powerCutStartedMs_ = 0;
  uint32_t powerRestoredMs_ = 0;
  bool hasPowerCycled_ = false;
  void setPump(bool on, uint32_t now);
};
int sensorIndex(const char* id);
const char* sensorName(size_t index);
const char* relayName(Relay relay);
const char* modeName(Mode mode);
bool validCalendar(int year, int month, int day, int hour, int minute, int second);
bool lightsScheduled(const Clock& clock);
uint64_t windowKey(const Clock& clock);
}
