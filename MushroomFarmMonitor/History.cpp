#include "History.h"

namespace farm {
bool History::append(const Record& r) {
  if (size == HISTORY_CAPACITY) { ++dropped; return false; }
  records[size++] = r;
  return true;
}
void History::collect(size_t i, const ControlCore& core, const Clock& c) {
  if (i >= SENSOR_COUNT || !core.sensors[i].seen) return;
  const auto& s = core.sensors[i];
  Record r;
  r.sensor = static_cast<uint8_t>(i);
  r.window = windowKey(c);
  r.localEpoch = c.valid ? c.localEpoch : 0;
  r.uptimeSeconds = static_cast<uint32_t>(c.uptimeMs / 1000);
  r.temperature = s.temperature;
  r.humidity = s.humidity;
  r.mistOn = core.relays[i].on;
  bool replaced = false;
  for (size_t j = size; j > frozen; --j) {
    Record& previous = records[j - 1];
    if (previous.sensor == i && previous.treatment == 0 && previous.window == r.window) {
      previous = r;
      replaced = true;
      break;
    }
  }
  if (!replaced) append(r);
  if (!core.relays[Lights].on) return;
  const size_t treatmentStart = i * 2;
  const size_t available = i == 2 ? 1 : 2;
  for (size_t k = 0; k < available && (k + 1) * 3 <= s.spectroCount; ++k) {
    const size_t t = treatmentStart + k;
    if (!treatmentSeen_[t] || treatmentWindow_[t] != r.window) {
      treatmentSeen_[t] = true;
      treatmentWindow_[t] = r.window;
      treatmentSets_[t] = 0;
    }
    if (treatmentSets_[t] >= SPECTRO_SETS_PER_WINDOW) continue;
    r.treatment = static_cast<uint8_t>(t + 1);
    r.set = treatmentSets_[t];
    for (size_t n = 0; n < 3; ++n) r.spectro[n] = s.spectro[k * 3 + n];
    if (append(r)) ++treatmentSets_[t];
  }
}
void History::freeze() { if (!frozen) frozen = size; }
void History::commit() {
  if (!frozen) return;
  const size_t remaining = size - frozen;
  for (size_t i = 0; i < remaining; ++i) records[i] = records[i + frozen];
  size = remaining;
  frozen = 0;
}
uint32_t crc32Update(uint32_t crc, const uint8_t* bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}
}
