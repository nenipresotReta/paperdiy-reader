#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Small diagnostic journal only. It never grants ownership or authorizes I/O.
namespace usb_storage {
enum class DiagnosticStage:uint32_t {None,Boot,Prepare,RawInitialize,HostOwned,Read,Write,Sync,Yield,Eject,Release,Inactive};
const char* diagnosticStageName(DiagnosticStage stage);
struct DiagnosticRecord {
  uint32_t sequence,stage,readCalls,writeCalls,lba,offset,length,usbStackLow;
};
struct Diagnostics {
  uint32_t resetReason=0;
  bool previousValid=false;
  DiagnosticRecord previous{},current{};
};
// Call early in setup to capture the previous valid RTC record before new work.
// Also initialized lazily if a diagnostic or USB entry arrives first.
void initializeDiagnostics();
Diagnostics diagnostics();
void diagnosticMark(DiagnosticStage stage,uint32_t lba=0,uint32_t offset=0,uint32_t length=0,uint32_t usbStackLow=0);

namespace diagnostic_detail {
constexpr uint32_t MAGIC=0x31444755,VERSION=1;
struct Slot {uint32_t magic,version;DiagnosticRecord record;uint32_t checksum;};
struct Retained {Slot slots[2];};
static_assert(sizeof(Slot)==44,"USB diagnostic RTC schema");
inline uint32_t checksum(const Slot& slot){
  uint32_t crc=0xffffffffu;const auto* bytes=reinterpret_cast<const uint8_t*>(&slot);
  for(size_t i=0;i<offsetof(Slot,checksum);++i){crc^=bytes[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
  return ~crc;
}
inline bool valid(const Slot& slot){return slot.magic==MAGIC&&slot.version==VERSION&&slot.record.sequence&&
  slot.record.stage<=uint32_t(DiagnosticStage::Inactive)&&slot.checksum==checksum(slot);}
inline int newest(const Retained& retained){
  const bool a=valid(retained.slots[0]),b=valid(retained.slots[1]);
  if(!a)return b?1:-1;if(!b)return 0;
  return int32_t(retained.slots[1].record.sequence-retained.slots[0].record.sequence)>0?1:0;
}
inline uint32_t increment(uint32_t value){return value==UINT32_MAX?value:value+1;}
// Caller serializes access. Publish magic last; a torn new slot cannot erase
// the older valid evidence. RTC retention across power loss is not promised.
inline void publish(Retained& retained,DiagnosticRecord& record){
  const int previous=newest(retained);const uint32_t sequence=previous<0?0:retained.slots[previous].record.sequence;
  record.sequence=sequence+1;if(!record.sequence)record.sequence=1;
  Slot next{};next.magic=MAGIC;next.version=VERSION;next.record=record;next.checksum=checksum(next);
  volatile Slot& destination=retained.slots[previous==0?1:0];
  destination.magic=0;
  destination.version=next.version;
  destination.record.sequence=next.record.sequence;destination.record.stage=next.record.stage;
  destination.record.readCalls=next.record.readCalls;destination.record.writeCalls=next.record.writeCalls;
  destination.record.lba=next.record.lba;destination.record.offset=next.record.offset;
  destination.record.length=next.record.length;destination.record.usbStackLow=next.record.usbStackLow;
  destination.checksum=next.checksum;
  destination.magic=MAGIC;
}
inline void mark(DiagnosticRecord& record,DiagnosticStage stage,uint32_t lba,uint32_t offset,uint32_t length,uint32_t stack){
  record.stage=uint32_t(stage);
  if(stage==DiagnosticStage::Read||stage==DiagnosticStage::Write||stage==DiagnosticStage::Sync){
    record.lba=lba;record.offset=offset;record.length=length;
  }
  if(stage==DiagnosticStage::Read)record.readCalls=increment(record.readCalls);
  if(stage==DiagnosticStage::Write)record.writeCalls=increment(record.writeCalls);
  if(stack&&(!record.usbStackLow||stack<record.usbStackLow))record.usbStackLow=stack;
}
}
}
