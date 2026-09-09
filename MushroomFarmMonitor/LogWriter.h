#pragma once
#include "History.h"

namespace farm {
class LogStorage {
public:
  virtual ~LogStorage() = default;
  virtual bool ready() const = 0;
  virtual bool create(const char* path) = 0; // exclusive create, never overwrite
  virtual bool append(const char* path, const char* data, size_t length) = 0;
  virtual size_t read(const char* path, size_t offset, uint8_t* buffer, size_t length) = 0;
  virtual int64_t fileSize(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual void remove(const char* path) = 0;
};
class LogWriter {
public:
  LogWriter(History& history, LogStorage& storage) : history_(history), storage_(storage) {}
  void setBootId(uint32_t a, uint32_t b) { bootA_ = a; bootB_ = b; }
  bool start();
  void step(); // bounded work: one CSV row or one verification block
  bool busy() const;
  bool failed() const { return state_ == State::Failed; }
  uint32_t completed = 0;
  const char* error = "";
  char lastCompleted[96] = {};
private:
  enum class State { Idle, Temperature, Spectro, VerifyTemperature, VerifySpectro, Commit, Failed };
  State state_ = State::Idle;
  History& history_;
  LogStorage& storage_;
  uint32_t bootA_ = 0, bootB_ = 0, sequence_ = 0;
  size_t index_ = 0, verifyOffset_ = 0;
  size_t bytes_[2] = {};
  uint32_t crc_[2] = {}, verifyCrc_ = 0;
  char base_[80] = {};
  char temp_[2][104] = {}, final_[2][104] = {}, marker_[104] = {}, markerTemp_[104] = {};
  bool ownTemp_[2] = {}, ownFinal_[2] = {}, ownMarkerTemp_ = false;
  bool write(unsigned file, const char* text);
  bool spectralRow(size_t first, char* output, size_t capacity);
  void fail(const char* reason);
  void verify(unsigned file);
};
void formatRecordTime(const Record& record, char* date, size_t dateSize,
                      char* time, size_t timeSize);
}
