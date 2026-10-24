#include "device_loop_trace.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace device_loop_trace;
Record sample(uint32_t at=7){return {0,at,uint32_t(Stage::AfterIdle),uint32_t(Command::SdHash),94,3,2,5,4,1};}
int main(){
  assert(checksum(sample())==0x19091905u); // Independent Python zlib.crc32, little-endian 10 words.
  Retained retained={};assert(newest(retained)==-1);
  publish(retained,sample());assert(newest(retained)==0&&retained.slots[0].record.sequence==1);
  publish(retained,sample(8));assert(newest(retained)==1&&retained.slots[1].record.sequence==2);
  assert(retained.slots[1].record.acceptedHigh==2&&retained.slots[1].record.droppedHigh==4);
  const Retained baseline=retained;
  // Every bit of a record/checksum is covered. The valid prior slot survives.
  for(size_t byte=0;byte<sizeof(Slot);++byte)for(unsigned bit=0;bit<8;++bit){
    retained=baseline;reinterpret_cast<uint8_t*>(&retained.slots[1])[byte]^=uint8_t(1u<<bit);
    assert(!valid(retained.slots[1])&&newest(retained)==0);
  }
  retained=baseline;retained.slots[1].magic=0;assert(newest(retained)==0);
  save(retained.slots[1].record,sample(9));assert(newest(retained)==0); // Torn publication.
  retained=baseline;retained.slots[1].record.sequence=UINT32_MAX;retained.slots[1].checksum=checksum(retained.slots[1].record);
  retained.slots[0].record.sequence=UINT32_MAX-1;retained.slots[0].checksum=checksum(retained.slots[0].record);
  assert(newest(retained)==1);publish(retained,sample(10));
  assert(newest(retained)==0&&retained.slots[0].record.sequence==0);
  const auto old=retained.slots[1];publish(retained,sample(11));
  assert(newest(retained)==1&&retained.slots[1].record.sequence==1&&old.record.sequence==UINT32_MAX);
  assert(classify("SD_HASH 2f61",12)==Command::SdHash&&classify("SD_HASHX",8)==Command::Other);
  assert(classify("STATUS",6)==Command::Status&&classify(nullptr,0)==Command::Other);
  assert(classify("FIXTURE_REMOVE",14)==Command::FixtureRemove);
  // No-op build has no RTC/Console dependency and accepts the same call sites.
  mark(Stage::Boot);{CommandScope scope("STATUS",6);}
  std::cout<<"RTC loop trace: CRC coverage, torn publication, prior slot, sequence wrap and numeric command classification passed\n";
}
