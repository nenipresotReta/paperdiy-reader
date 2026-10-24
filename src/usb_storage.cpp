#include "usb_storage.h"
#include "usb_transport.h"
#include <atomic>

#ifdef ARDUINO
#include <Arduino.h>
#endif

#if defined(ARDUINO) && !ARDUINO_USB_MODE && CONFIG_TINYUSB_MSC_ENABLED
#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>
#include <ff.h>
#include <diskio.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "usb_ram_disk.h"
#include "storage_validation.h"

namespace usb_storage {
namespace {
Core core;
Status snapshot;
std::atomic<bool> ownership{false};
std::atomic<Error> callbackError{Error::NotReady};
std::atomic<SemaphoreHandle_t> mutex{nullptr};
uint8_t drive=0xff;
uint8_t* ram=nullptr;
bool driverReleased=true;
BackendReason lastBackendReason=BackendReason::None;
BackendReason backendFailureReason(void*){return lastBackendReason;}
#ifndef PAPERDIY_USB_RAM_DISK
bool sameCard(){
  if(storage_validation::cardEventPending()){lastBackendReason=BackendReason::CardEdge;return false;}
  if(digitalRead(48)!=LOW){lastBackendReason=BackendReason::CardAbsent;return false;}
  return true;
}
bool rawReady(){if(drive==0xff){lastBackendReason=BackendReason::DriverUnavailable;return false;}return sameCard();}
#endif
struct Lock {
  SemaphoreHandle_t handle;
  bool acquired;
  Lock():handle(mutex.load()),acquired(handle&&xSemaphoreTake(handle,0)==pdTRUE){}
  ~Lock(){if(acquired)xSemaphoreGive(handle);}
  explicit operator bool() const {return acquired;}
};
bool backendRead(void*,uint32_t lba,uint8_t* bytes){
#ifdef PAPERDIY_USB_RAM_DISK
  std::memcpy(bytes,ram+size_t(lba)*SECTOR_SIZE,SECTOR_SIZE);return true;
#else
  if(!rawReady())return false;
  const bool ok=sd_read_raw(drive,bytes,lba);if(!sameCard())return false;
  if(!ok)lastBackendReason=BackendReason::RawRead;return ok;
#endif
}
bool backendWrite(void*,uint32_t lba,const uint8_t* bytes){
#ifdef PAPERDIY_USB_RAM_DISK
  std::memcpy(ram+size_t(lba)*SECTOR_SIZE,bytes,SECTOR_SIZE);return true;
#else
  // Pinned SDK's raw API takes a mutable pointer but does not modify writes.
  if(!rawReady())return false;
  const bool ok=sd_write_raw(drive,const_cast<uint8_t*>(bytes),lba);if(!sameCard())return false;
  if(!ok)lastBackendReason=BackendReason::RawWrite;return ok;
#endif
}
bool backendReadMany(void*,uint32_t lba,uint8_t* bytes,uint32_t count){
  if(count<2||count>CALLBACK_BUDGET/SECTOR_SIZE)return false;
#ifdef PAPERDIY_USB_RAM_DISK
  std::memcpy(bytes,ram+size_t(lba)*SECTOR_SIZE,size_t(count)*SECTOR_SIZE);return true;
#else
  if(!rawReady())return false;
  const bool ok=disk_read(drive,bytes,lba,count)==RES_OK;if(!sameCard())return false;
  if(!ok)lastBackendReason=BackendReason::RawRead;return ok;
#endif
}
bool backendWriteMany(void*,uint32_t lba,const uint8_t* bytes,uint32_t count){
  if(count<2||count>CALLBACK_BUDGET/SECTOR_SIZE)return false;
#ifdef PAPERDIY_USB_RAM_DISK
  std::memcpy(ram+size_t(lba)*SECTOR_SIZE,bytes,size_t(count)*SECTOR_SIZE);return true;
#else
  if(!rawReady())return false;
  const bool ok=disk_write(drive,bytes,lba,count)==RES_OK;if(!sameCard())return false;
  if(!ok)lastBackendReason=BackendReason::RawWrite;return ok;
#endif
}
bool backendSync(void*){
  diagnosticMark(DiagnosticStage::Sync);
#ifdef PAPERDIY_USB_RAM_DISK
  return true;
#else
  if(!rawReady())return false;
  const bool ok=disk_ioctl(drive,CTRL_SYNC,nullptr)==RES_OK;if(!sameCard())return false;
  if(!ok)lastBackendReason=BackendReason::RawSync;return ok;
#endif
}
bool releaseDriver(){
  diagnosticMark(DiagnosticStage::Release);
  if(drive!=0xff){if(sdcard_uninit(drive)!=0)return false;drive=0xff;}
  if(ram){heap_caps_free(ram);ram=nullptr;}
  driverReleased=true;return true;
}
void recordError(){callbackError.store(core.status().error,std::memory_order_relaxed);}
}
bool available(){return usb_transport::storageSupported();}
bool ramTestMode(){
#ifdef PAPERDIY_USB_RAM_DISK
  return true;
#else
  return false;
#endif
}
bool begin(uint32_t frequency){
  if(!available()||ownership.load())return false;
  if(!mutex.load())mutex.store(xSemaphoreCreateMutex());
  Lock lock;if(!lock||core.status().state!=State::Inactive)return false;
  // Also fail closed when the caller forgot SD.end(). The application guard
  // must independently block every local SD user before reaching this point.
  if(SD.cardType()!=CARD_NONE)return false;
  diagnosticMark(DiagnosticStage::RawInitialize);
  uint32_t count=0;
  lastBackendReason=BackendReason::None;
  ownership.store(true);driverReleased=false;
#ifdef PAPERDIY_USB_RAM_DISK
  (void)frequency;
  ram=static_cast<uint8_t*>(heap_caps_malloc(size_t(RAM_SECTORS)*SECTOR_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!ram)ram=static_cast<uint8_t*>(heap_caps_malloc(size_t(RAM_SECTORS)*SECTOR_SIZE,MALLOC_CAP_8BIT));
  if(ram&&initializeRamDisk(ram,size_t(RAM_SECTORS)*SECTOR_SIZE))count=RAM_SECTORS;
#else
  // SPI was configured by the application's SD owner. No FAT mount/format API
  // is used here. Use the actual allocated drive number, never assume drive 0.
  if(sameCard()&&frequency&&frequency<=25000000){
    drive=sdcard_init(47,&SPI,int(frequency));
    if(drive!=0xff&&!(disk_initialize(drive)&(STA_NOINIT|STA_NODISK))&&sameCard()&&sdcard_sector_size(drive)==SECTOR_SIZE)
      count=sdcard_num_sectors(drive);
  }
#endif
  if(!count||!core.begin({nullptr,backendRead,backendWrite,backendSync,backendReadMany,backendWriteMany,backendFailureReason},count)){
    // An initialization failure does not advertise medium present.
    if(releaseDriver())ownership.store(false);
    snapshot={};snapshot.error=Error::NotReady;
    return false;
  }
  snapshot=core.status();callbackError.store(Error::None);
  diagnosticMark(DiagnosticStage::HostOwned);return true;
}
void poll(){
  Lock lock;if(!lock)return;
  snapshot=core.status();
  if(snapshot.state==State::Ejected&&!driverReleased){
    if(releaseDriver())ownership.store(false);
    else {
      core.noteFailure(Error::Sync,BackendReason::DriverRelease);snapshot=core.status();
      snapshot.state=State::Fault;snapshot.error=Error::Sync;
    }
  }
}
Status status(){return snapshot;}
bool hostOwnsCard(){return ownership.load();}
bool requestExit(){
  Lock lock;if(!lock)return !ownership.load();
  core.requestExit();snapshot=core.status();
  return !ownership.load();
}
bool finish(){
  Lock lock;if(!lock||ownership.load()||!driverReleased)return false;
  if(!core.reset())return false;
  snapshot=core.status();diagnosticMark(DiagnosticStage::Inactive);return true;
}
namespace transport_detail {
bool ready(){Lock lock;return lock&&core.ready();}
uint32_t sectors(){Lock lock;return lock&&core.ready()?core.status().sectors:0;}
int32_t read(uint32_t lba,uint32_t offset,void* bytes,uint32_t length){
  diagnosticMark(DiagnosticStage::Read,lba,offset,length,uxTaskGetStackHighWaterMark(nullptr));
  int32_t n=0;
  {Lock lock;if(lock){n=core.read(lba,offset,bytes,length);recordError();}}
  // TinyUSB's highest-priority task may immediately retry a zero result or
  // receive another bulk request. Let the idle task run, after releasing the
  // storage mutex, on success, failure and contention alike.
  diagnosticMark(DiagnosticStage::Yield,0,0,0,uxTaskGetStackHighWaterMark(nullptr));
  vTaskDelay(1);return n;
}
int32_t write(uint32_t lba,uint32_t offset,const void* bytes,uint32_t length){
  diagnosticMark(DiagnosticStage::Write,lba,offset,length,uxTaskGetStackHighWaterMark(nullptr));
  int32_t n=0;
  {Lock lock;if(lock){n=core.write(lba,offset,bytes,length);recordError();}}
  diagnosticMark(DiagnosticStage::Yield,0,0,0,uxTaskGetStackHighWaterMark(nullptr));
  vTaskDelay(1);return n;
}
bool startStop(uint8_t power,bool start,bool eject){
  Lock lock;if(!lock)return false;
  const bool ok=core.startStop(power,start,eject);recordError();
  if(ok&&core.status().state==State::Ejected)diagnosticMark(DiagnosticStage::Eject);
  return ok;
}
bool synchronize(uint32_t lba,uint16_t count){Lock lock;if(!lock)return false;const bool ok=core.synchronize(lba,count);recordError();return ok;}
bool preventRemoval(uint8_t prevent){Lock lock;if(!lock)return false;const bool ok=core.preventRemoval(prevent);recordError();return ok;}
Error lastError(){return callbackError.load(std::memory_order_relaxed);}
}
}
#else
namespace usb_storage {
bool available(){return false;}
bool ramTestMode(){return false;}
bool begin(uint32_t){return false;}
void poll(){}
bool requestExit(){return true;}
Status status(){return {};}
bool hostOwnsCard(){return false;}
bool finish(){return true;}
}
#endif
