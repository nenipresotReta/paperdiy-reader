#include "usb_storage.h"
#include "usb_transport.h"
#include "storage_validation.h"
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

FakeSD SD;
SPIClass SPI;
namespace {
bool locked=false,busy=false,allocationFailed=false,initFailed=false,syncFailed=false,releaseFailed=false;
bool cardAbsent=false;
uint32_t initializes=0,uninitializes=0,reads=0,writes=0,syncs=0,allocations=0,frees=0;
uint32_t manyReads=0,manyWrites=0;
uint32_t callbackYields=0;
bool failMany=false,edgeOnMany=false;
constexpr uint8_t DRIVE=7;
std::array<uint8_t,16*512> card{};
}
namespace usb_transport {bool storageSupported(){return true;}}
int digitalRead(int pin){assert(pin==48);return cardAbsent?1:0;}
SemaphoreHandle_t xSemaphoreCreateMutex(){return &locked;}
int xSemaphoreTake(SemaphoreHandle_t handle,int ticks){assert(handle==&locked&&ticks==0);if(busy||locked)return 0;locked=true;return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t handle){assert(handle==&locked&&locked);locked=false;}
void vTaskDelay(uint32_t ticks){assert(ticks==1&&!locked);++callbackYields;}
uint32_t uxTaskGetStackHighWaterMark(void* task){assert(!task);return 1700;}
void* heap_caps_malloc(size_t bytes,unsigned){++allocations;return allocationFailed?nullptr:std::malloc(bytes);}
void heap_caps_free(void* p){++frees;std::free(p);}
uint8_t sdcard_init(uint8_t cs,SPIClass* spi,int frequency){assert(cs==47&&spi==&SPI&&frequency==4000000&&!SD.mounted);++initializes;return DRIVE;}
uint8_t sdcard_uninit(uint8_t drive){assert(drive==DRIVE);++uninitializes;return releaseFailed?1:0;}
uint8_t disk_initialize(uint8_t drive){assert(drive==DRIVE);return initFailed?1:0;}
int disk_ioctl(uint8_t drive,uint8_t command,void* bytes){assert(drive==DRIVE&&command==0&&!bytes);++syncs;return syncFailed?1:0;}
uint32_t sdcard_sector_size(uint8_t drive){assert(drive==DRIVE);return 512;}
uint32_t sdcard_num_sectors(uint8_t drive){assert(drive==DRIVE);return 16;}
bool sd_read_raw(uint8_t drive,uint8_t* bytes,uint32_t lba){assert(drive==DRIVE&&lba<16&&!SD.mounted);++reads;std::memcpy(bytes,card.data()+lba*512,512);return true;}
bool sd_write_raw(uint8_t drive,uint8_t* bytes,uint32_t lba){assert(drive==DRIVE&&lba<16&&!SD.mounted);++writes;std::memcpy(card.data()+lba*512,bytes,512);return true;}
int disk_read(uint8_t drive,uint8_t* bytes,uint32_t lba,uint32_t count){assert(drive==DRIVE&&count>=2&&count<=8&&lba+count<=16&&!SD.mounted);++manyReads;std::memcpy(bytes,card.data()+size_t(lba)*512,size_t(count)*512);if(edgeOnMany)storage_validation::signalCardEvent();return failMany?1:0;}
int disk_write(uint8_t drive,const uint8_t* bytes,uint32_t lba,uint32_t count){assert(drive==DRIVE&&count>=2&&count<=8&&lba+count<=16&&!SD.mounted);++manyWrites;std::memcpy(card.data()+size_t(lba)*512,bytes,failMany?512:size_t(count)*512);if(edgeOnMany)storage_validation::signalCardEvent();return failMany?1:0;}
int main(){
  using namespace usb_storage;
  assert(available()&&!hostOwnsCard());
  assert(!transport_detail::ready());assert(transport_detail::sectors()==0);
  SD.mounted=true;assert(!begin(4000000));assert(!hostOwnsCard()&&!initializes);
  SD.mounted=false;
#ifdef PAPERDIY_USB_RAM_DISK
  allocationFailed=true;assert(!begin(4000000));assert(!hostOwnsCard());assert(allocations==2&&!initializes);
  allocationFailed=false;assert(ramTestMode());
#else
  assert(!ramTestMode());assert(!begin(0));assert(!hostOwnsCard()&&!initializes);
  initFailed=true;assert(!begin(4000000));assert(!hostOwnsCard()&&initializes==1&&uninitializes==1);
  initFailed=false;
  cardAbsent=true;assert(!begin(4000000));assert(!hostOwnsCard()&&initializes==1);cardAbsent=false;
  storage_validation::signalCardEvent();assert(!begin(4000000));assert(!hostOwnsCard()&&initializes==1);
  storage_validation::acknowledgeCardEvent();
#endif
  assert(begin(4000000));assert(hostOwnsCard()&&transport_detail::ready());
  assert(!begin(4000000));assert(!finish());
  uint8_t bytes[512]={0xa9};
  const auto initialYields=callbackYields;
  assert(transport_detail::write(1,0,bytes,512)==512);
  assert(callbackYields==initialYields+1&&!locked);
  uint8_t bulk[4096]={0x4c},out[4096]={};
  assert(transport_detail::write(2,0,bulk,sizeof(bulk))==4096);
  assert(transport_detail::read(2,0,out,sizeof(out))==4096&&!std::memcmp(bulk,out,sizeof(out)));
  assert(callbackYields==initialYields+3);
  busy=true;
  assert(transport_detail::write(2,0,bytes,512)==0);
  assert(transport_detail::read(2,0,bytes,512)==0);
  assert(callbackYields==initialYields+5&&!locked); // Busy retries yield too.
  const auto diagnostic=diagnostics();
  assert(diagnostic.current.readCalls==2&&diagnostic.current.writeCalls==3);
  assert(diagnostic.current.stage==uint32_t(DiagnosticStage::Yield)&&diagnostic.current.usbStackLow==1700);
  assert(!requestExit()&&!finish());
  poll();assert(hostOwnsCard());busy=false;
  assert(!requestExit());assert(status().state==State::ExitRequested);
  assert(transport_detail::synchronize());assert(hostOwnsCard());
  assert(transport_detail::preventRemoval(1));assert(!transport_detail::startStop(0,false,true));
  assert(transport_detail::preventRemoval(0));
#ifndef PAPERDIY_USB_RAM_DISK
  syncFailed=true;assert(!transport_detail::startStop(0,false,true));poll();
  assert(status().state==State::Fault&&hostOwnsCard());
  assert(status().firstError==Error::Sync&&status().backendReason==BackendReason::RawSync);
  assert(transport_detail::read(0,0,bytes,sizeof(bytes))<0);poll();
  assert(status().error==Error::NotReady&&status().firstError==Error::Sync&&status().backendReason==BackendReason::RawSync);syncFailed=false;
#endif
  assert(transport_detail::startStop(0,false,true));
  // Protocol completion withdraws media, but app still cannot mount FAT until
  // poll serializes raw-driver removal and clears conservative ownership.
  assert(hostOwnsCard()&&!transport_detail::ready());assert(!finish());
  const auto rejectedYields=callbackYields;
  assert(transport_detail::read(0,0,bytes,sizeof(bytes))<0);
  assert(transport_detail::write(0,0,bytes,sizeof(bytes))<0);
  assert(callbackYields==rejectedYields+2&&!locked);
#ifndef PAPERDIY_USB_RAM_DISK
  releaseFailed=true;poll();assert(hostOwnsCard()&&status().state==State::Fault&&!finish());releaseFailed=false;
#endif
  poll();assert(!hostOwnsCard()&&status().state==State::Ejected);
  assert(finish()&&status().state==State::Inactive);
#ifdef PAPERDIY_USB_RAM_DISK
  assert(initializes==0&&uninitializes==0&&reads==0&&writes==0&&syncs==0&&manyReads==0&&manyWrites==0&&frees==1);
#else
  assert(initializes==2&&uninitializes==3&&writes==1&&syncs==3&&manyReads==1&&manyWrites==1);
  assert(begin(4000000));failMany=true;
  assert(transport_detail::write(0,0,bulk,sizeof(bulk))<0);poll();
  assert(hostOwnsCard()&&status().state==State::Fault&&status().bytesWritten==0);
  assert(status().firstError==Error::Write&&status().backendReason==BackendReason::RawWrite);
  assert(transport_detail::write(0,0,bytes,sizeof(bytes))<0);poll();
  assert(status().error==Error::NotReady&&status().firstError==Error::Write&&status().backendReason==BackendReason::RawWrite);
  failMany=false;assert(transport_detail::startStop(0,false,true));poll();assert(finish());
  assert(begin(4000000));edgeOnMany=true;
  assert(transport_detail::read(0,0,out,sizeof(out))<0);poll();
  assert(hostOwnsCard()&&status().state==State::Fault&&status().bytesRead==0);
  assert(status().firstError==Error::Read&&status().backendReason==BackendReason::CardEdge);
  assert(transport_detail::read(0,0,out,sizeof(out))<0);poll();
  assert(status().error==Error::NotReady&&status().firstError==Error::Read&&status().backendReason==BackendReason::CardEdge);
  // Test teardown emulates the explicit recovery boundary. Normal app code
  // never acknowledges an edge while host ownership is retained.
  edgeOnMany=false;storage_validation::acknowledgeCardEvent();
  assert(transport_detail::startStop(0,false,true));poll();assert(finish());
  assert(status().firstError==Error::None&&status().backendReason==BackendReason::None);
  assert(begin(4000000));cardAbsent=true;const auto absentReads=manyReads;
  assert(transport_detail::read(0,0,out,sizeof(out))<0);poll();
  assert(manyReads==absentReads&&hostOwnsCard()&&status().firstError==Error::Read&&status().backendReason==BackendReason::CardAbsent);
  cardAbsent=false;assert(transport_detail::startStop(0,false,true));poll();assert(finish());
#endif
  // Finish the clean session first, then prove the physical-card edge latch
  // revokes the old raw geometry across removal/reinsertion. The app must not
  // acknowledge that edge while the host still owns the block device.
#ifndef PAPERDIY_USB_RAM_DISK
  assert(begin(4000000));const auto beforeWrites=writes,beforeReads=reads;
  cardAbsent=true;storage_validation::signalCardEvent();cardAbsent=false;
  assert(transport_detail::write(0,0,bytes,512)<0);
  assert(writes==beforeWrites&&reads==beforeReads&&hostOwnsCard());
  const auto beforeManyReads=manyReads,beforeManyWrites=manyWrites;
  assert(transport_detail::read(0,0,out,sizeof(out))<0&&transport_detail::write(0,0,bulk,sizeof(bulk))<0);
  assert(manyReads==beforeManyReads&&manyWrites==beforeManyWrites);
  assert(!transport_detail::startStop(0,false,true));
  poll();assert(status().state==State::Fault&&hostOwnsCard()&&!finish());
#else
  assert(begin(4000000));cardAbsent=true;storage_validation::signalCardEvent();
  assert(transport_detail::write(0,0,bytes,512)==512);
  assert(transport_detail::startStop(0,false,true));poll();assert(finish());
#endif
  std::puts("usb_storage production adapter tests passed");
}
