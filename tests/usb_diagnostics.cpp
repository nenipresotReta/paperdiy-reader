#include "usb_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
using namespace usb_storage;
using namespace usb_storage::diagnostic_detail;
static_assert(std::is_trivial<Retained>::value,"RTC journal must have no boot constructor");
int main(){
  Retained journal{};assert(newest(journal)==-1);
  DiagnosticRecord record{};record.stage=uint32_t(DiagnosticStage::Read);record.lba=123;
  publish(journal,record);assert(record.sequence==1&&newest(journal)==0);
  const auto old=journal;
  mark(record,DiagnosticStage::Write,5,7,4096,1800);publish(journal,record);
  assert(newest(journal)==1&&record.writeCalls==1&&record.lba==5&&record.offset==7&&record.length==4096);
  const auto complete=journal;
  // Every torn prefix of a new slot leaves the old committed slot readable.
  for(size_t bytes=0;bytes<sizeof(Slot);++bytes){
    auto torn=old;std::memcpy(&torn.slots[1],&complete.slots[1],bytes);
    assert(newest(torn)==0&&torn.slots[newest(torn)].record.lba==123);
  }
  auto corrupt=complete;corrupt.slots[1].record.length^=1;assert(newest(corrupt)==0);
  corrupt=complete;corrupt.slots[1].version=2;assert(newest(corrupt)==0);
  corrupt=complete;corrupt.slots[1].record.stage=UINT32_MAX;corrupt.slots[1].checksum=checksum(corrupt.slots[1]);assert(newest(corrupt)==0);
  record.sequence=UINT32_MAX;Slot final{};final.magic=MAGIC;final.version=VERSION;final.record=record;final.checksum=checksum(final);
  journal={};journal.slots[0]=final;publish(journal,record);assert(record.sequence==1&&newest(journal)==1);
  record={};record.readCalls=UINT32_MAX;record.writeCalls=UINT32_MAX;
  mark(record,DiagnosticStage::Read,8,0,512,1900);mark(record,DiagnosticStage::Write,9,1,1024,1700);
  mark(record,DiagnosticStage::Yield,0,0,0,1800);
  assert(record.readCalls==UINT32_MAX&&record.writeCalls==UINT32_MAX&&record.usbStackLow==1700);
  assert(record.lba==9&&record.offset==1&&record.length==1024);
  initializeDiagnostics();auto boot=diagnostics();assert(!boot.previousValid&&boot.current.stage==uint32_t(DiagnosticStage::Boot));
  diagnosticMark(DiagnosticStage::Read,1,2,3,1600);auto runtime=diagnostics();
  assert(runtime.current.readCalls==1&&runtime.current.lba==1&&runtime.current.usbStackLow==1600);
  assert(!std::strcmp(diagnosticStageName(DiagnosticStage::RawInitialize),"raw_init"));
  puts("USB RTC diagnostics: CRC/torn slots/schema/wrap/counters/stack/volatile identity: PASS");
}
