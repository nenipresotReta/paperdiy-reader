#pragma once
// Main application task only. Numeric RTC evidence; no logging, allocation,
// filesystem, network, USB-register access, filenames or command payloads.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace device_loop_trace {
enum class Stage:uint32_t {
  Boot=1,LoopBegin,BeforeM5,AfterM5,BeforeIndicator,AfterIndicator,
  BeforeUsb,AfterUsb,BeforeStorage,AfterStorage,BeforeBattery,AfterBattery,
  BeforeDiagnostics,AfterDiagnostics,BeforeIdle,AfterIdle,CommandStart,CommandEnd
};
enum class Command:uint32_t {None,Status,UsbStatus,Screen,SdHash,FixtureInstall,FixtureRemove,PowerStatus,Other};
struct Record {
  uint32_t sequence,milliseconds,stage,command,commandBytes;
  uint32_t acceptedLow,acceptedHigh,droppedLow,droppedHigh,abortedFrames;
};
struct Slot {uint32_t magic,checksum;Record record;};
struct Retained {Slot slots[2];};
static_assert(sizeof(Record)==40&&sizeof(Slot)==48&&sizeof(Retained)==96,"RTC trace layout");
constexpr uint32_t MAGIC=0x4c545231; // LTR1; change on any record-layout change.
inline uint32_t crcWord(uint32_t crc,uint32_t word){
  for(unsigned byte=0;byte<4;++byte){
    crc^=(word>>(byte*8))&255;
    for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));
  }return crc;
}
inline uint32_t checksum(const Record& r){
  uint32_t crc=0xffffffff;
  crc=crcWord(crc,r.sequence);crc=crcWord(crc,r.milliseconds);crc=crcWord(crc,r.stage);
  crc=crcWord(crc,r.command);crc=crcWord(crc,r.commandBytes);
  crc=crcWord(crc,r.acceptedLow);crc=crcWord(crc,r.acceptedHigh);
  crc=crcWord(crc,r.droppedLow);crc=crcWord(crc,r.droppedHigh);crc=crcWord(crc,r.abortedFrames);
  return crc^0xffffffff;
}
inline Record load(const volatile Record& r){
  return {r.sequence,r.milliseconds,r.stage,r.command,r.commandBytes,
    r.acceptedLow,r.acceptedHigh,r.droppedLow,r.droppedHigh,r.abortedFrames};
}
inline void save(volatile Record& to,const Record& r){
  to.sequence=r.sequence;to.milliseconds=r.milliseconds;to.stage=r.stage;
  to.command=r.command;to.commandBytes=r.commandBytes;
  to.acceptedLow=r.acceptedLow;to.acceptedHigh=r.acceptedHigh;
  to.droppedLow=r.droppedLow;to.droppedHigh=r.droppedHigh;to.abortedFrames=r.abortedFrames;
}
inline bool valid(const volatile Slot& slot){
  if(slot.magic!=MAGIC)return false;
  const Record record=load(slot.record);const uint32_t crc=slot.checksum;
  return slot.magic==MAGIC&&record.stage>=uint32_t(Stage::Boot)&&record.stage<=uint32_t(Stage::CommandEnd)&&
    record.command<=uint32_t(Command::Other)&&record.commandBytes<=2056&&checksum(record)==crc;
}
inline int newest(const volatile Retained& retained){
  const bool a=valid(retained.slots[0]),b=valid(retained.slots[1]);
  if(!a)return b?1:-1;if(!b)return 0;
  const uint32_t difference=retained.slots[1].record.sequence-retained.slots[0].record.sequence;
  return difference&&difference<0x80000000u?1:0;
}
inline void publish(volatile Retained& retained,Record record){
  const int previous=newest(retained);const unsigned target=previous==0?1:0;
  record.sequence=previous<0?1:retained.slots[previous].record.sequence+1;
  auto& slot=retained.slots[target];slot.magic=0;
  save(slot.record,record);slot.checksum=checksum(record);
  std::atomic_thread_fence(std::memory_order_release);
  slot.magic=MAGIC; // A torn new slot cannot invalidate the previous slot.
}
inline bool equal(const char* p,size_t n,const char* expected){return p&&std::strlen(expected)==n&&!std::memcmp(p,expected,n);}
inline Command classify(const char* p,size_t n){
  if(equal(p,n,"STATUS"))return Command::Status;
  if(equal(p,n,"USB_STATUS"))return Command::UsbStatus;
  if(equal(p,n,"SCREEN"))return Command::Screen;
  if(equal(p,n,"SD_HASH")||(p&&n>=8&&!std::memcmp(p,"SD_HASH ",8)))return Command::SdHash;
  if(equal(p,n,"FIXTURE_INSTALL"))return Command::FixtureInstall;
  if(equal(p,n,"FIXTURE_REMOVE"))return Command::FixtureRemove;
  if(equal(p,n,"POWER_STATUS"))return Command::PowerStatus;
  return Command::Other;
}
} // namespace device_loop_trace

#if defined(ARDUINO) && defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
#include <Arduino.h>
#include <esp_attr.h>
#include "console.h"
namespace device_loop_trace {
// Exactly one inclusion in app.cpp. A plain POD has no startup initializer.
RTC_NOINIT_ATTR volatile Retained retained;
namespace runtime {static Record current={};}
inline void mark(Stage stage){
  auto& record=runtime::current;record.milliseconds=millis();record.stage=uint32_t(stage);
  const auto stats=Console.writer().stats();
  record.acceptedLow=uint32_t(stats.accepted);record.acceptedHigh=uint32_t(stats.accepted>>32);
  record.droppedLow=uint32_t(stats.dropped);record.droppedHigh=uint32_t(stats.dropped>>32);
  record.abortedFrames=stats.abortedFrames;publish(retained,record);
}
class CommandScope {
 public:
  CommandScope(const char* text,size_t bytes){
    runtime::current.command=uint32_t(classify(text,bytes));
    runtime::current.commandBytes=uint32_t(bytes);mark(Stage::CommandStart);
  }
  ~CommandScope(){mark(Stage::CommandEnd);}
  CommandScope(const CommandScope&)=delete;CommandScope& operator=(const CommandScope&)=delete;
};
}
#else
namespace device_loop_trace {
inline void mark(Stage){}
class CommandScope {
 public:CommandScope(const char*,size_t){}
};
}
#endif
