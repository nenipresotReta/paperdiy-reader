#include "usb_storage_core.h"
#include "usb_ram_disk.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
using namespace usb_storage;
struct Disk {
  std::vector<uint8_t> bytes;
  uint32_t reads=0,writes=0,syncs=0,manyReads=0,manyWrites=0,lastCount=0;
  bool readFail=false,writeFail=false,syncFail=false,manyReadFail=false,manyWriteFail=false;
  explicit Disk(uint32_t sectors):bytes(size_t(sectors)*SECTOR_SIZE,0xa5){}
  Backend backend(bool many=false){Backend b{this,
    [](void* p,uint32_t lba,uint8_t* out){auto& d=*static_cast<Disk*>(p);++d.reads;assert(uint64_t(lba)*SECTOR_SIZE+SECTOR_SIZE<=d.bytes.size());if(d.readFail)return false;std::memcpy(out,d.bytes.data()+size_t(lba)*SECTOR_SIZE,SECTOR_SIZE);return true;},
    [](void* p,uint32_t lba,const uint8_t* in){auto& d=*static_cast<Disk*>(p);++d.writes;assert(uint64_t(lba)*SECTOR_SIZE+SECTOR_SIZE<=d.bytes.size());if(d.writeFail)return false;std::memcpy(d.bytes.data()+size_t(lba)*SECTOR_SIZE,in,SECTOR_SIZE);return true;},
    [](void* p){auto& d=*static_cast<Disk*>(p);++d.syncs;return !d.syncFail;}};
    if(many){
      b.readMany=[](void* p,uint32_t lba,uint8_t* out,uint32_t count){auto& d=*static_cast<Disk*>(p);++d.manyReads;d.lastCount=count;assert(count>=2&&count<=8&&uint64_t(lba+count)*512<=d.bytes.size());std::memcpy(out,d.bytes.data()+size_t(lba)*512,d.manyReadFail?512:size_t(count)*512);return !d.manyReadFail;};
      b.writeMany=[](void* p,uint32_t lba,const uint8_t* in,uint32_t count){auto& d=*static_cast<Disk*>(p);++d.manyWrites;d.lastCount=count;assert(count>=2&&count<=8&&uint64_t(lba+count)*512<=d.bytes.size());std::memcpy(d.bytes.data()+size_t(lba)*512,in,d.manyWriteFail?512:size_t(count)*512);return !d.manyWriteFail;};
    }
    return b;
  }
};
void ownership(){
  Disk disk(16);Core c;
  assert(!c.ready()&&!c.owns());assert(c.requestExit());
  assert(!c.begin({},16));assert(!c.begin(disk.backend(),0));
  assert(c.begin(disk.backend(),16));assert(c.ready()&&c.owns());
  assert(!c.begin(disk.backend(),16));assert(!c.reset());
  assert(!c.requestExit());assert(c.status().state==State::ExitRequested);
  // There is intentionally no elapsed-time API granting local ownership.
  for(unsigned i=0;i<1000;++i){assert(!c.requestExit());assert(c.owns());}
  assert(!c.startStop(0,false,false));assert(c.owns());
  assert(c.preventRemoval(1));assert(!c.startStop(0,false,true));
  assert(c.status().error==Error::Prevented&&c.owns()&&disk.syncs==0);
  assert(!c.preventRemoval(2));assert(c.status().removalPrevented);
  assert(c.preventRemoval(0));assert(c.synchronize());assert(c.owns());
  assert(!c.startStop(1,false,true));assert(c.owns());
  assert(c.startStop(0,false,true));assert(!c.ready()&&!c.owns());
  assert(c.status().state==State::Ejected&&disk.syncs==2);
  assert(c.requestExit());assert(!c.startStop(0,true,true));
  uint8_t b;assert(c.read(0,0,&b,1)<0);assert(c.write(0,0,&b,1)<0);
  assert(c.reset());assert(c.status().state==State::Inactive);
}
void unalignedAndBounds(){
  Disk disk(32);Core c;assert(c.begin(disk.backend(),32));
  std::vector<uint8_t> input(8192),output(8192);
  for(size_t i=0;i<input.size();++i)input[i]=uint8_t(i*31+7);
  assert(c.write(1,509,input.data(),9)==9);
  assert(disk.bytes[512+508]==0xa5&&disk.bytes[512+518]==0xa5);
  assert(c.read(1,509,output.data(),9)==9);
  assert(!std::memcmp(input.data(),output.data(),9));
  assert(c.write(2,0,input.data(),512)==512);
  const auto oldReads=disk.reads;assert(c.write(3,0,input.data(),512)==512);
  assert(disk.reads==oldReads); // full-sector write avoids read-modify-write
  assert(c.write(0,511,input.data(),8192)==CALLBACK_BUDGET);
  assert(c.read(0,511,output.data(),8192)==CALLBACK_BUDGET);
  assert(!std::memcmp(input.data(),output.data(),CALLBACK_BUDGET));
  assert(c.read(31,511,output.data(),1)==1);
  assert(c.read(32,0,nullptr,0)==0);
  const auto reads=disk.reads,writes=disk.writes;
  assert(c.read(31,511,output.data(),2)<0);
  assert(c.write(31,511,input.data(),2)<0);
  assert(c.read(UINT32_MAX,UINT32_MAX,output.data(),1)<0);
  assert(c.write(0,0,input.data(),UINT32_MAX)<0);
  assert(c.read(0,0,nullptr,1)<0);
  assert(c.write(0,0,nullptr,1)<0);
  assert(disk.reads==reads&&disk.writes==writes);
  assert(c.ready()&&c.owns());
  // Normalize offsets larger than one sector without 32-bit arithmetic wrap.
  assert(c.write(0,1025,input.data(),1)==1);assert(disk.bytes[1025]==input[0]);
}
void failuresRetainOwnership(){
  for(unsigned kind=0;kind<3;++kind){
    Disk disk(8);Core c;assert(c.begin(disk.backend(),8));uint8_t b[512]={};
    if(kind==0){disk.readFail=true;assert(c.read(0,0,b,sizeof(b))<0);}
    if(kind==1){disk.writeFail=true;assert(c.write(0,0,b,sizeof(b))<0);}
    if(kind==2){disk.syncFail=true;assert(!c.startStop(0,false,true));}
    assert(c.status().state==State::Fault&&c.owns()&&!c.ready());
    assert(!c.requestExit());assert(!c.reset());
    disk.syncFail=true;assert(!c.startStop(0,false,true));assert(c.owns());
    disk.readFail=disk.writeFail=disk.syncFail=false;
    assert(c.startStop(0,false,true));assert(!c.owns());
  }
}
void firstFailureSurvivesRetries(){
  Disk disk(8);Core c;auto backend=disk.backend();
  backend.failureReason=[](void*){return BackendReason::RawRead;};
  assert(c.begin(backend,8));disk.readFail=true;uint8_t bytes[512]{};
  assert(c.read(0,0,bytes,sizeof(bytes))<0);
  assert(c.status().firstError==Error::Read&&c.status().backendReason==BackendReason::RawRead);
  // Subsequent retry and failed eject alter the latest error, but must never
  // replace the cause of the transition into Fault or grant ownership back.
  assert(c.read(0,0,bytes,sizeof(bytes))<0&&c.status().error==Error::NotReady);
  disk.syncFail=true;assert(!c.startStop(0,false,true));
  assert(c.status().error==Error::Sync&&c.status().firstError==Error::Read&&c.status().backendReason==BackendReason::RawRead&&c.owns());
  disk.syncFail=false;assert(c.startStop(0,false,true));
  assert(c.status().firstError==Error::Read);assert(c.reset());
  assert(c.status().firstError==Error::None&&c.status().backendReason==BackendReason::None);
}
void randomizedCoverage(){
  Disk disk(64);Core c;assert(c.begin(disk.backend(),64));auto reference=disk.bytes;
  std::array<uint8_t,1024> bytes{},out{};uint32_t random=9;
  auto next=[&](){random=random*1664525u+1013904223u;return random;};
  for(unsigned i=0;i<2000;++i){
    const uint32_t length=next()%1024,position=next()%(reference.size()-length);
    for(auto& b:bytes)b=uint8_t(next()>>24);
    assert(c.write(position/512,position%512,bytes.data(),length)==int32_t(length));
    std::memcpy(reference.data()+position,bytes.data(),length);
    assert(c.read(position/512,position%512,out.data(),length)==int32_t(length));
    assert(!std::memcmp(bytes.data(),out.data(),length));
    assert(reference==disk.bytes);
  }
}
void ramImage(){
  std::vector<uint8_t> ram(RAM_SECTORS*SECTOR_SIZE);
  assert(!initializeRamDisk(nullptr,ram.size()));assert(!initializeRamDisk(ram.data(),ram.size()-1));
  assert(initializeRamDisk(ram.data(),ram.size()));
  auto le16=[&](size_t p){return uint16_t(ram[p]|uint16_t(ram[p+1])<<8);};
  assert(le16(11)==512&&ram[13]==1&&le16(19)==RAM_SECTORS);
  assert(ram[510]==0x55&&ram[511]==0xaa);
  assert(!std::memcmp(ram.data()+512,ram.data()+1024,512));
  assert(!std::memcmp(ram.data()+1536,"README  TXT",11));
  assert(ram[1536+26]==2&&le16(1536+28)>0&&le16(1536+28)<512);
  assert(!std::memcmp(ram.data()+2560,"PaperDIY disposable",19));
}
void multiSectorBudgetAndPartialFailures(){
  Disk disk(32);Core core;assert(core.begin(disk.backend(true),32));
  std::vector<uint8_t> bytes(8192,0x7b),out(8192);
  assert(core.write(1,0,bytes.data(),8192)==4096);
  assert(disk.manyWrites==1&&disk.lastCount==8&&disk.writes==0&&disk.reads==0);
  assert(core.read(1,0,out.data(),8192)==4096);
  assert(disk.manyReads==1&&disk.lastCount==8&&disk.reads==0);
  assert(!std::memcmp(bytes.data(),out.data(),4096));
  assert(core.write(10,1,bytes.data(),4096)==4096);
  assert(disk.manyWrites==2&&disk.lastCount==7&&disk.writes==2&&disk.reads==2);
  assert(disk.bytes[10*512]==0xa5&&disk.bytes[10*512+4097]==0xa5);
  assert(core.read(10,1,out.data(),4096)==4096&&!std::memcmp(bytes.data(),out.data(),4096));
  const auto beforeReads=disk.manyReads,beforeWrites=disk.manyWrites;
  assert(core.write(31,0,bytes.data(),8192)<0&&core.read(UINT32_MAX,UINT32_MAX,out.data(),8)<0);
  assert(disk.manyReads==beforeReads&&disk.manyWrites==beforeWrites);
  for(bool write:{false,true}){
    Disk failing(16);Core c;assert(c.begin(failing.backend(true),16));
    if(write){failing.manyWriteFail=true;assert(c.write(0,0,bytes.data(),4096)<0);assert(c.status().bytesWritten==0);assert(failing.bytes[0]==0x7b&&failing.bytes[512]==0xa5);}
    else {failing.manyReadFail=true;assert(c.read(0,0,out.data(),4096)<0);assert(c.status().bytesRead==0);}
    assert(c.owns()&&!c.ready()&&c.status().state==State::Fault&&!c.requestExit());
    assert(c.startStop(0,false,true));
  }
}
}
int main(){ownership();unalignedAndBounds();failuresRetainOwnership();firstFailureSurvivesRetries();randomizedCoverage();ramImage();multiSectorBudgetAndPartialFailures();std::puts("usb_storage tests passed");}
