#include "ControlCore.h"
#include "History.h"
#include "LogWriter.h"
#include <cassert>
#include <climits>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>

using namespace farm;
static unsigned scenarios = 0;
static void pass(const char* name) { ++scenarios; std::cout << "PASS " << name << '\n'; }
static Clock clockAt(uint8_t hour = 17, uint8_t minute = 0, uint32_t day = 20260909) {
  Clock c;
  c.valid = true; c.localEpoch = 1788973200; c.hour = hour; c.minute = minute; c.dayKey = day;
  return c;
}
class MemoryDisk : public LogStorage {
public:
  std::map<std::string, std::string> files;
  bool online = true, shortWrite = false, corruptRead = false, markerFailure = false;
  int failRenameAt = 0, renames = 0;
  bool ready() const override { return online; }
  bool create(const char* p) override {
    if (!online || files.count(p)) return false;
    files[p] = ""; return true;
  }
  bool append(const char* p, const char* bytes, size_t n) override {
    if (!online || !files.count(p)) return false;
    if (markerFailure && std::string(p).find(".ok.tmp") != std::string::npos) return false;
    if (shortWrite) { files[p].append(bytes, n / 2); shortWrite = false; return false; }
    files[p].append(bytes, n); return true;
  }
  size_t read(const char* p, size_t offset, uint8_t* buffer, size_t n) override {
    if (!online || !files.count(p) || offset + n > files[p].size()) return 0;
    std::memcpy(buffer, files[p].data() + offset, n);
    if (corruptRead && n) buffer[0] ^= 1;
    return n;
  }
  int64_t fileSize(const char* p) override {
    return online && files.count(p) ? static_cast<int64_t>(files[p].size()) : -1;
  }
  bool rename(const char* a, const char* b) override {
    if (++renames == failRenameAt || !online || !files.count(a) || files.count(b)) return false;
    files[b] = files[a]; files.erase(a); return true;
  }
  void remove(const char* p) override { if (online) files.erase(p); }
};
static void finish(LogWriter& log) {
  int limit = 10000;
  while (log.busy() && --limit) log.step();
  assert(limit > 0);
}
static void addReading(History& h, ControlCore& c, Clock clock, int base = 10) {
  c.hardwareReady = true;
  int32_t v[] = {base+1,base+2,base+3,base+4,base+5,base+6};
  assert(c.accept(0, 25, 70, v, 6, 1, clock));
  c.evaluate(1, clock);
  h.collect(0, c, clock);
}
int main() {
  {
    ControlCore c; c.hardwareReady = true; Clock t = clockAt();
    assert(c.accept(0,25,74,nullptr,0,0,t)); c.evaluate(0,t); assert(c.relays[Mist1].on);
    assert(c.accept(0,25,80,nullptr,0,1,t)); c.evaluate(1,t); assert(c.relays[Mist1].on);
    assert(c.accept(0,25,85,nullptr,0,2,t)); c.evaluate(2,t); assert(!c.relays[Mist1].on);
    assert(c.accept(0,25,75,nullptr,0,3,t)); c.evaluate(3,t); assert(!c.relays[Mist1].on);
    pass("humidity hysteresis and exact thresholds");
  }
  {
    ControlCore c; c.hardwareReady = true; Clock t = clockAt();
    assert(c.accept(2,25,99,nullptr,0,0,t)); c.setMode(Mist3,Mode::ManualOn,0); c.evaluate(1,t);
    assert(c.relays[Mist3].on); c.setMode(Mist3,Mode::ManualOff,2);
    assert(c.accept(2,25,10,nullptr,0,2,t)); c.evaluate(2,t); assert(!c.relays[Mist3].on);
    c.resetModes(); c.evaluate(3,t); assert(c.relays[Mist3].on);
    pass("mist maker 3 manual ON, OFF and return to AUTO");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt();
    assert(c.accept(0,25,50,nullptr,0,0,t)); assert(c.fresh(0,0));
    c.evaluate(29999,t); assert(c.relays[Mist1].on);
    c.evaluate(30000,t); assert(!c.relays[Mist1].on);
    c.evaluate(0,t); assert(!c.fresh(0,0)); // a long-dead node must not revive after millis wraps
    assert(c.accept(0,25,50,nullptr,0,UINT32_MAX-100,t));
    assert(c.fresh(0,50)); assert(!c.fresh(0,30100));
    pass("stale shutdown, first millisecond reading, millis wraparound and latched stale state");
  }
  {
    ControlCore c; Clock t=clockAt(); int32_t v[7]={};
    assert(!c.accept(0,std::numeric_limits<float>::quiet_NaN(),50,nullptr,0,1,t));
    assert(!c.accept(0,25,101,nullptr,0,1,t)); assert(!c.accept(0,25,50,v,7,1,t));
    assert(!c.accept(3,25,50,nullptr,0,1,t)); assert(!c.sensors[0].seen);
    assert(sensorIndex("sensor100")==-1 && sensorIndex(nullptr)==-1 && sensorIndex("sensor3")==2);
    pass("invalid readings and oversized spectral arrays rejected without mutation");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt();
    c.accept(0,21,80,nullptr,0,0,t); c.evaluate(0,t); assert(c.relays[Exhaust].on);
    c.accept(0,22,80,nullptr,0,1,t); c.evaluate(1,t); assert(!c.relays[Exhaust].on);
    c.accept(0,29,80,nullptr,0,2,t); c.evaluate(2,t); assert(!c.relays[Exhaust].on);
    c.accept(0,30,80,nullptr,0,3,t); c.evaluate(3,t); assert(c.relays[Exhaust].on);
    c.evaluate(30003,t); assert(!c.relays[Exhaust].on);
    pass("preserved outside-range exhaust rule and stale exclusion");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt(14,0);
    c.evaluate(100,t); assert(c.relays[Sprinkler].on && c.sprinklerDayChanged);
    c.evaluate(30099,t); assert(c.relays[Sprinkler].on);
    c.evaluate(30100,t); assert(!c.relays[Sprinkler].on);
    c.evaluate(40000,t); assert(!c.relays[Sprinkler].on);
    ControlCore reboot; reboot.hardwareReady=true; reboot.lastSprinklerDay=c.lastSprinklerDay;
    reboot.evaluate(1,t); assert(!reboot.relays[Sprinkler].on);
    t.dayKey=20260908; reboot.evaluate(2,t); assert(!reboot.relays[Sprinkler].on);
    t.dayKey=20260910; reboot.evaluate(3,t); assert(reboot.relays[Sprinkler].on);
    pass("scheduled pump duration, reset deduplication and backward date correction");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt(14,0);
    c.setMode(Sprinkler,Mode::ManualOff,0); c.evaluate(1,t); assert(!c.relays[Sprinkler].on);
    c.setMode(Sprinkler,Mode::ManualOn,100);
    c.setMode(Sprinkler,Mode::ManualOn,20000); c.evaluate(30100,t);
    assert(!c.relays[Sprinkler].on && c.relays[Sprinkler].mode==Mode::ManualOff);
    c.evaluate(40000,t); assert(!c.relays[Sprinkler].on);
    c.setMode(Sprinkler,Mode::ManualOn,UINT32_MAX-100); c.evaluate(30000,t);
    assert(!c.relays[Sprinkler].on);
    pass("manual pump override, repeated ON does not extend timeout, rollover");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt(14,0);
    c.evaluate(0,t); assert(c.relays[Sprinkler].on); t.valid=false; c.evaluate(1,t);
    assert(!c.relays[Sprinkler].on && !c.relays[Lights].on);
    c.setMode(Sprinkler,Mode::ManualOn,2); c.evaluate(2,t); assert(c.relays[Sprinkler].on);
    c.hardwareReady=false; c.evaluate(3,t); assert(!c.relays[Sprinkler].on);
    pass("untrusted clock disables schedules; hardware loss clears commanded states");
  }
  {
    ControlCore c; c.hardwareReady=true; Clock t=clockAt(16);
    c.evaluate(0,t); assert(c.relays[Lights].on);
    c.setMode(Lights,Mode::ManualOff,1); c.evaluate(1,t); assert(!c.relays[Lights].on);
    c.resetModes(); t.hour=6; c.evaluate(2,t); assert(c.relays[Lights].on);
    t.hour=7; c.evaluate(3,t); assert(!c.relays[Lights].on);
    pass("overnight lighting boundaries and manual OFF overrides schedule");
  }
  {
    ControlCore c; c.hardwareReady=true;
    c.monitorNodes(59999); assert(!c.c3PowerCut); c.monitorNodes(60000); assert(c.c3PowerCut);
    c.monitorNodes(64999); assert(c.c3PowerCut); c.monitorNodes(65000); assert(!c.c3PowerCut);
    c.monitorNodes(124999); assert(!c.c3PowerCut); c.monitorNodes(125000); assert(c.c3PowerCut);
    pass("C3 startup grace, cut duration and recovery cooldown");
  }
  {
    assert(validCalendar(2028,2,29,23,59,59)); assert(!validCalendar(2026,2,29,12,0,0));
    assert(!validCalendar(2026,4,31,12,0,0)); assert(!validCalendar(2026,9,9,24,0,0));
    Clock a=clockAt(), b=a; b.generation++; assert(windowKey(a)!=windowKey(b));
    b=a; b.valid=false; assert(windowKey(a)!=windowKey(b));
    b=a; b.localEpoch+=31536000; assert(windowKey(a)!=windowKey(b));
    pass("calendar validation and distinct windows across clock corrections and years");
  }
  {
    History h; ControlCore c; Clock t=clockAt();
    addReading(h,c,t,10); addReading(h,c,t,20); addReading(h,c,t,30); addReading(h,c,t,40);
    assert(h.size==7); // one latest DHT + 3 sets for each of two treatments
    h.freeze(); addReading(h,c,t,50); assert(h.size==8); // no fourth spectral set after freeze
    assert(h.records[0].spectro[0]==0 && h.records[1].spectro[0]==11);
    h.commit(); assert(h.size==1 && h.records[0].treatment==0);
    pass("DHT aggregation, first three spectral sets, immutable export snapshot");
  }
  {
    History h; ControlCore c; Clock t=clockAt(); c.hardwareReady=true;
    c.setMode(Lights,Mode::ManualOff,0); addReading(h,c,t);
    assert(h.size==1 && h.records[0].treatment==0);
    c.setMode(Lights,Mode::ManualOn,1); addReading(h,c,t); assert(h.size==3);
    pass("spectral logging follows actual commanded lights state");
  }
  {
    History h; ControlCore c; Clock t=clockAt();
    for (size_t i=0;i<HISTORY_CAPACITY+10;++i) { t.localEpoch+=300; addReading(h,c,t); }
    assert(h.size==HISTORY_CAPACITY && h.dropped>0);
    const uint64_t first=h.records[0].window;
    t.localEpoch+=300; addReading(h,c,t); assert(h.records[0].window==first);
    pass("full buffer reports omissions and preserves older pending readings");
  }
  {
    History h; ControlCore c; Clock t=clockAt(); addReading(h,c,t,10); addReading(h,c,t,20);
    MemoryDisk disk; LogWriter log(h,disk); log.setBootId(1,2); assert(log.start());
    t.localEpoch+=300; addReading(h,c,t,30); const size_t tail=h.size-h.frozen;
    finish(log); assert(!log.failed() && log.completed==1 && h.size==tail && h.frozen==0);
    bool csv=false,marker=false;
    for (const auto& f:disk.files) {
      if (f.first.find("-spectro.csv")!=std::string::npos) { csv=true; assert(f.second.find("\"11 12 13 21 22 23\"")!=std::string::npos); }
      if (f.first.find(".ok")!=std::string::npos) marker=true;
      assert(f.first.find(".tmp")==std::string::npos);
    }
    assert(csv && marker && disk.files.size()==3);
    pass("verified two-file CSV transaction, reading order and new arrivals preserved");
  }
  {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; LogWriter log(h,disk); disk.shortWrite=true;
    assert(!log.start()); const size_t retained=h.size; assert(log.failed() && h.frozen==retained);
    assert(log.start()); finish(log); assert(h.size==0 && log.completed==1);
    pass("short SD write retains history and a retry commits it");
  }
  {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; LogWriter log(h,disk); assert(log.start()); disk.corruptRead=true; finish(log);
    assert(log.failed() && h.size>0 && h.frozen==h.size);
    assert(std::string(log.error).find("checksum")!=std::string::npos);
    disk.corruptRead=false; assert(log.start()); finish(log); assert(h.size==0);
    pass("same-length SD corruption detected by CRC and retried");
  }
  for (int renameFailure=1;renameFailure<=3;++renameFailure) {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; disk.failRenameAt=renameFailure; LogWriter log(h,disk); assert(log.start()); finish(log);
    assert(log.failed() && h.size>0); assert(disk.files.empty());
    disk.failRenameAt=0; assert(log.start()); finish(log); assert(h.size==0);
    pass("CSV/commit-marker rename failure retains data and cleans only owned files");
  }
  {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; disk.markerFailure=true; LogWriter log(h,disk); assert(log.start()); finish(log);
    assert(log.failed() && h.size>0 && disk.files.empty());
    disk.markerFailure=false; assert(log.start()); finish(log); assert(h.size==0);
    pass("commit marker write failure retains history");
  }
  {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; LogWriter log(h,disk); assert(log.start()); disk.online=false; finish(log);
    assert(log.failed() && h.size>0); disk.online=true; assert(log.start()); finish(log); assert(h.size==0);
    pass("SD disconnect and reconnect retry");
  }
  {
    History h; ControlCore c; addReading(h,c,clockAt());
    MemoryDisk disk; LogWriter log(h,disk); log.setBootId(1,2); assert(log.start());
    const auto saved=disk.files; // Build the same first-attempt names in a fresh logger.
    History h2=h; h2.frozen=0; LogWriter collision(h2,disk); collision.setBootId(1,2);
    assert(!collision.start()); assert(disk.files==saved);
    pass("filename collision never deletes pre-existing files");
  }
  {
    const char* bytes="123456789";
    assert((crc32Update(0xFFFFFFFFu,reinterpret_cast<const uint8_t*>(bytes),9)^0xFFFFFFFFu)==0xCBF43926u);
    pass("CRC32 standard reference vector");
  }
  std::cout << scenarios << " scenarios passed\n";
}
