#pragma once
#include "ControlCore.h"

namespace farm {
struct Record {
  uint64_t window = 0;
  uint32_t localEpoch = 0; // zero means no valid RTC at acquisition
  uint32_t uptimeSeconds = 0;
  float temperature = 0;
  float humidity = 0;
  int32_t spectro[3] = {};
  uint8_t sensor = 0;
  uint8_t treatment = 0; // 0 = temp/humidity; 1..5 = spectral treatment
  uint8_t set = 0;
  bool mistOn = false;
};
class History {
public:
  Record records[HISTORY_CAPACITY] = {};
  size_t size = 0;
  size_t frozen = 0; // Immutable prefix while a CSV transaction is pending.
  uint32_t dropped = 0;
  void collect(size_t sensor, const ControlCore& core, const Clock& clock);
  void freeze();
  void commit();
private:
  uint64_t treatmentWindow_[5] = {};
  uint8_t treatmentSets_[5] = {};
  bool treatmentSeen_[5] = {};
  bool append(const Record& record);
};
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length);
}
