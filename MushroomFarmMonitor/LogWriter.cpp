#include "LogWriter.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace farm {
void formatRecordTime(const Record& r, char* date, size_t nd, char* time, size_t nt) {
  if (!r.localEpoch) {
    std::snprintf(date, nd, "No_Date");
    std::snprintf(time, nt, "up_%lu:%02lu:%02lu", static_cast<unsigned long>(r.uptimeSeconds / 3600),
        static_cast<unsigned long>((r.uptimeSeconds / 60) % 60), static_cast<unsigned long>(r.uptimeSeconds % 60));
    return;
  }
  time_t epoch = r.localEpoch;
  tm t = {};
  gmtime_r(&epoch, &t); // localEpoch intentionally encodes Manila wall-clock fields.
  std::snprintf(date, nd, "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  const int hour = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
  std::snprintf(time, nt, "%02d:%02d:%02d %s", hour, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
}
bool LogWriter::busy() const { return state_ != State::Idle && state_ != State::Failed; }
void LogWriter::fail(const char* reason) {
  error = reason;
  state_ = State::Failed;
  // Only this attempt's unique names are removed. Uncommitted RAM stays frozen.
  for (unsigned f = 0; f < 2; ++f) {
    if (ownTemp_[f]) storage_.remove(temp_[f]);
    if (ownFinal_[f]) storage_.remove(final_[f]);
  }
  if (ownMarkerTemp_) storage_.remove(markerTemp_);
}
bool LogWriter::write(unsigned f, const char* text) {
  const size_t n = std::strlen(text);
  if (!storage_.append(temp_[f], text, n)) { fail("SD write incomplete; history retained"); return false; }
  bytes_[f] += n;
  crc_[f] = crc32Update(crc_[f], reinterpret_cast<const uint8_t*>(text), n);
  return true;
}
bool LogWriter::start() {
  if (busy() || !history_.size) return false;
  if (!storage_.ready()) { error = "SD unavailable; history retained"; return false; }
  history_.freeze();
  ++sequence_;
  char stamp[24] = {};
  const Record& first = history_.records[0];
  if (first.localEpoch) {
    time_t epoch = first.localEpoch;
    tm t = {};
    gmtime_r(&epoch, &t);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &t);
  } else std::snprintf(stamp, sizeof(stamp), "uptime%lu", static_cast<unsigned long>(first.uptimeSeconds));
  std::snprintf(base_, sizeof(base_), "/logs/%s-%08lx%08lx-%lu", stamp,
      static_cast<unsigned long>(bootA_), static_cast<unsigned long>(bootB_), static_cast<unsigned long>(sequence_));
  for (unsigned f = 0; f < 2; ++f) {
    std::snprintf(temp_[f], sizeof(temp_[f]), "%s-%s.tmp", base_, f ? "spectro" : "temp");
    std::snprintf(final_[f], sizeof(final_[f]), "%s-%s.csv", base_, f ? "spectro" : "temp");
    bytes_[f] = 0;
    crc_[f] = 0xFFFFFFFFu;
    ownTemp_[f] = ownFinal_[f] = false;
  }
  std::snprintf(marker_, sizeof(marker_), "%s.ok", base_);
  std::snprintf(markerTemp_, sizeof(markerTemp_), "%s.ok.tmp", base_);
  ownMarkerTemp_ = false;
  for (unsigned f = 0; f < 2; ++f) {
    if (!storage_.create(temp_[f])) { fail("Cannot create SD log files; history retained"); return false; }
    ownTemp_[f] = true;
  }
  state_ = State::Temperature;
  error = "";
  index_ = 0;
  return write(0, "Date,Time,Sensor,Temperature,Humidity,Status\n") &&
         write(1, "Date,Time,Treatment1,Treatment2,Treatment3,Treatment4,Treatment5\n");
}
bool LogWriter::spectralRow(size_t first, char* out, size_t capacity) {
  const auto& r = history_.records[first];
  char date[24], time[32];
  formatRecordTime(r, date, sizeof(date), time, sizeof(time));
  size_t used = static_cast<size_t>(std::snprintf(out, capacity, "%s,%s", date, time));
  if (used >= capacity) return false;
  for (uint8_t t = 1; t <= 5; ++t) {
    if (used + 4 >= capacity) return false;
    out[used++] = ',';
    out[used++] = '"';
    bool hasValues = false;
    // Preserve set order: s1v1 s1v2 s1v3 s2v1 ... (not transposed).
    for (uint8_t set = 0; set < SPECTRO_SETS_PER_WINDOW; ++set) {
      for (size_t j = 0; j < history_.frozen; ++j) {
        const auto& e = history_.records[j];
        if (e.window != r.window || e.treatment != t || e.set != set) continue;
        for (unsigned k = 0; k < 3; ++k) {
          const int n = std::snprintf(out + used, capacity - used, "%s%ld", hasValues ? " " : "",
                                      static_cast<long>(e.spectro[k]));
          if (n < 0 || static_cast<size_t>(n) >= capacity - used) return false;
          used += static_cast<size_t>(n);
          hasValues = true;
        }
      }
    }
    out[used++] = '"';
    out[used] = '\0';
  }
  if (used + 2 >= capacity) return false;
  out[used++] = '\n';
  out[used] = '\0';
  return true;
}
void LogWriter::verify(unsigned f) {
  if (storage_.fileSize(temp_[f]) != static_cast<int64_t>(bytes_[f])) {
    fail("SD file size mismatch; history retained"); return;
  }
  uint8_t block[512];
  const size_t remaining = bytes_[f] - verifyOffset_;
  const size_t wanted = remaining < sizeof(block) ? remaining : sizeof(block);
  if (wanted) {
    if (storage_.read(temp_[f], verifyOffset_, block, wanted) != wanted) {
      fail("SD verification read failed; history retained"); return;
    }
    verifyCrc_ = crc32Update(verifyCrc_, block, wanted);
    verifyOffset_ += wanted;
  }
  if (verifyOffset_ == bytes_[f]) {
    if (verifyCrc_ != crc_[f]) { fail("SD checksum mismatch; history retained"); return; }
    verifyOffset_ = 0;
    verifyCrc_ = 0xFFFFFFFFu;
    state_ = f == 0 ? State::VerifySpectro : State::Commit;
  }
}
void LogWriter::step() {
  if (!busy()) return;
  if (!storage_.ready()) { fail("SD disconnected; history retained"); return; }
  char row[768];
  if (state_ == State::Temperature) {
    while (index_ < history_.frozen && history_.records[index_].treatment) ++index_;
    if (index_ == history_.frozen) { index_ = 0; state_ = State::Spectro; return; }
    const auto& r = history_.records[index_++];
    char date[24], time[32];
    formatRecordTime(r, date, sizeof(date), time, sizeof(time));
    const int n = std::snprintf(row, sizeof(row), "%s,%s,%s,%.2f,%.2f,%s\n", date, time,
        sensorName(r.sensor), static_cast<double>(r.temperature), static_cast<double>(r.humidity), r.mistOn ? "ON" : "OFF");
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) fail("CSV row too long");
    else write(0, row);
  } else if (state_ == State::Spectro) {
    while (index_ < history_.frozen) {
      const size_t first = index_++;
      const auto& r = history_.records[first];
      if (!r.treatment) continue;
      bool seen = false;
      for (size_t j = 0; j < first; ++j)
        if (history_.records[j].treatment && history_.records[j].window == r.window) { seen = true; break; }
      if (seen) continue;
      if (!spectralRow(first, row, sizeof(row))) fail("Spectral CSV row too long");
      else write(1, row);
      return;
    }
    state_ = State::VerifyTemperature;
    verifyOffset_ = 0;
    verifyCrc_ = 0xFFFFFFFFu;
  } else if (state_ == State::VerifyTemperature) verify(0);
  else if (state_ == State::VerifySpectro) verify(1);
  else if (state_ == State::Commit) {
    for (unsigned f = 0; f < 2; ++f) {
      if (!storage_.rename(temp_[f], final_[f])) { fail("CSV rename failed; history retained"); return; }
      ownTemp_[f] = false;
      ownFinal_[f] = true;
    }
    const int n = std::snprintf(row, sizeof(row),
        "version=%s\nrecords=%lu\ntemp_bytes=%lu\ntemp_crc32=%08lx\nspectro_bytes=%lu\nspectro_crc32=%08lx\n",
        VERSION, static_cast<unsigned long>(history_.frozen), static_cast<unsigned long>(bytes_[0]),
        static_cast<unsigned long>(crc_[0] ^ 0xFFFFFFFFu), static_cast<unsigned long>(bytes_[1]),
        static_cast<unsigned long>(crc_[1] ^ 0xFFFFFFFFu));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row) || !storage_.create(markerTemp_)) {
      fail("Commit marker creation failed; history retained"); return;
    }
    ownMarkerTemp_ = true;
    if (!storage_.append(markerTemp_, row, static_cast<size_t>(n))) { fail("Commit marker write failed; history retained"); return; }
    uint8_t check[256];
    if (static_cast<size_t>(n) > sizeof(check) || storage_.fileSize(markerTemp_) != n ||
        storage_.read(markerTemp_, 0, check, n) != static_cast<size_t>(n) || std::memcmp(check, row, n) != 0) {
      fail("Commit marker verification failed; history retained"); return;
    }
    if (!storage_.rename(markerTemp_, marker_)) { fail("Commit marker rename failed; history retained"); return; }
    ownMarkerTemp_ = false;
    history_.commit();
    ++completed;
    std::snprintf(lastCompleted, sizeof(lastCompleted), "%s", base_);
    state_ = State::Idle;
    error = "";
  }
}
}
