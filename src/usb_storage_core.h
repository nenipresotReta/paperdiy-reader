#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// Portable protocol/ownership policy. The adapter serializes every call; no
// filesystem, allocation, UI, timing heuristic or SDK dependency belongs here.
namespace usb_storage {
constexpr uint32_t SECTOR_SIZE=512;
constexpr uint32_t CALLBACK_BUDGET=4096;
enum class State : uint8_t { Inactive, HostOwned, ExitRequested, Ejected, Fault };
enum class Error : uint8_t { None, NotReady, Range, Read, Write, Sync, Prevented, Unsupported };
enum class BackendReason : uint8_t { None,CardEdge,CardAbsent,RawRead,RawWrite,RawSync,DriverUnavailable,DriverRelease };
inline const char* backendReasonName(BackendReason reason){
  switch(reason){
    case BackendReason::CardEdge:return "card_edge";case BackendReason::CardAbsent:return "card_absent";
    case BackendReason::RawRead:return "raw_read";case BackendReason::RawWrite:return "raw_write";
    case BackendReason::RawSync:return "raw_sync";case BackendReason::DriverUnavailable:return "driver_unavailable";
    case BackendReason::DriverRelease:return "driver_release";default:return "none";
  }
}
struct Backend {
  void* context=nullptr;
  bool (*read)(void*,uint32_t,uint8_t*)=nullptr;
  bool (*write)(void*,uint32_t,const uint8_t*)=nullptr;
  bool (*sync)(void*)=nullptr;
  bool (*readMany)(void*,uint32_t,uint8_t*,uint32_t)=nullptr;
  bool (*writeMany)(void*,uint32_t,const uint8_t*,uint32_t)=nullptr;
  BackendReason (*failureReason)(void*)=nullptr;
  Backend(void* ctx=nullptr,bool (*r)(void*,uint32_t,uint8_t*)=nullptr,
          bool (*w)(void*,uint32_t,const uint8_t*)=nullptr,bool (*s)(void*)=nullptr,
          bool (*rm)(void*,uint32_t,uint8_t*,uint32_t)=nullptr,
          bool (*wm)(void*,uint32_t,const uint8_t*,uint32_t)=nullptr,
          BackendReason (*reason)(void*)=nullptr)
    :context(ctx),read(r),write(w),sync(s),readMany(rm),writeMany(wm),failureReason(reason){}
};
struct Status {
  State state=State::Inactive;
  Error error=Error::None;
  Error firstError=Error::None;
  BackendReason backendReason=BackendReason::None;
  uint32_t sectors=0;
  uint64_t bytesRead=0,bytesWritten=0;
  bool removalPrevented=false;
};
class Core {
 public:
  bool begin(Backend backend,uint32_t sectors){
    if(owns()||!sectors||!backend.read||!backend.write||!backend.sync)return false;
    backend_=backend;status_={};status_.state=State::HostOwned;status_.sectors=sectors;
    return true;
  }
  bool owns() const {return status_.state==State::HostOwned||status_.state==State::ExitRequested||status_.state==State::Fault;}
  bool ready() const {return status_.state==State::HostOwned||status_.state==State::ExitRequested;}
  Status status() const {return status_;}
  // Diagnostic latch only: does not change readiness, state or ownership.
  // Retain the first media/driver failure until this session is reset.
  void noteFailure(Error error,BackendReason reason){
    if(status_.firstError==Error::None){status_.firstError=error;status_.backendReason=reason;}
  }
  bool requestExit(){
    if(status_.state==State::HostOwned)status_.state=State::ExitRequested;
    return !owns();
  }
  // Only called after the adapter has removed the raw driver, after Ejected.
  bool reset(){if(owns())return false;status_={};backend_={};return true;}
  int32_t read(uint32_t lba,uint32_t offset,void* out,uint32_t length){return transfer(lba,offset,out,length,false);}
  int32_t write(uint32_t lba,uint32_t offset,const void* in,uint32_t length){return transfer(lba,offset,const_cast<void*>(in),length,true);}
  bool synchronize(uint32_t lba=0,uint16_t count=0){
    if(!owns()){status_.error=Error::NotReady;return false;}
    if(lba>=status_.sectors||(count&&uint64_t(lba)+count>status_.sectors)){status_.error=Error::Range;return false;}
    if(!backend_.sync(backend_.context)){fault(Error::Sync);return false;}
    return true;
  }
  bool preventRemoval(uint8_t prevent){
    // Persistent prevent values (2/3) and reserved bits are unsupported.
    if(prevent>1){status_.error=Error::Unsupported;return false;}
    if(!owns()){status_.error=Error::NotReady;return false;}
    status_.removalPrevented=prevent!=0;return true;
  }
  bool startStop(uint8_t power,bool start,bool loadEject){
    if(power){status_.error=Error::Unsupported;return false;}
    if(start){if(!ready()){status_.error=Error::NotReady;return false;}return true;}
    if(!loadEject){status_.error=Error::Unsupported;return false;}
    if(!owns()){status_.error=Error::NotReady;return false;}
    if(status_.removalPrevented){status_.error=Error::Prevented;return false;}
    if(!synchronize())return false;
    // Host explicitly relinquishes the medium only after accepted writes have
    // completed and the card reports ready. No elapsed-idle test grants this.
    status_.state=State::Ejected;status_.error=Error::None;return true;
  }
 private:
  void fault(Error error){
    noteFailure(error,backend_.failureReason?backend_.failureReason(backend_.context):BackendReason::None);
    status_.error=error;status_.state=State::Fault;
  }
  int32_t transfer(uint32_t lba,uint32_t offset,void* buffer,uint32_t length,bool writing){
    if(!ready()){status_.error=Error::NotReady;return -1;}
    const uint64_t address=uint64_t(lba)*SECTOR_SIZE+offset;
    const uint64_t capacity=uint64_t(status_.sectors)*SECTOR_SIZE;
    if((length&&!buffer)||address>capacity||uint64_t(length)>capacity-address){status_.error=Error::Range;return -1;}
    const uint32_t count=std::min(length,CALLBACK_BUDGET);
    auto* bytes=static_cast<uint8_t*>(buffer);
    uint8_t sector[SECTOR_SIZE];
    uint32_t done=0;
    while(done<count){
      const uint64_t position=address+done;
      const uint32_t index=uint32_t(position/SECTOR_SIZE),within=uint32_t(position%SECTOR_SIZE);
      const uint32_t sectors=(count-done)/SECTOR_SIZE;
      if(!within&&sectors>=2&&(writing?backend_.writeMany!=nullptr:backend_.readMany!=nullptr)){
        const bool ok=writing?backend_.writeMany(backend_.context,index,bytes+done,sectors):
          backend_.readMany(backend_.context,index,bytes+done,sectors);
        // A failed multi-sector operation may have changed a prefix. Report
        // failure for the command, retain ownership and never acknowledge an
        // invented partial success or permit a local FAT remount.
        if(!ok){fault(writing?Error::Write:Error::Read);return -1;}
        const uint32_t n=sectors*SECTOR_SIZE;
        if(writing)status_.bytesWritten+=n;else status_.bytesRead+=n;
        done+=n;continue;
      }
      const uint32_t n=std::min(count-done,SECTOR_SIZE-within);
      if(!writing||within||n!=SECTOR_SIZE){
        if(!backend_.read(backend_.context,index,sector)){fault(Error::Read);return -1;}
      }
      if(writing){
        const uint8_t* source=bytes+done;
        if(within||n!=SECTOR_SIZE){std::memcpy(sector+within,source,n);source=sector;}
        if(!backend_.write(backend_.context,index,source)){fault(Error::Write);return -1;}
        status_.bytesWritten+=n;
      }else {std::memcpy(bytes+done,sector+within,n);status_.bytesRead+=n;}
      done+=n;
    }
    return int32_t(done);
  }
  Backend backend_;
  Status status_;
};
}
