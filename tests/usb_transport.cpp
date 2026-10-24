#include "usb_storage.h"
#include "usb_transport.h"
#include <esp32-hal-tinyusb.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <Arduino.h>
MockCdc Serial;

namespace {
usb_storage::Core core;
tinyusb_descriptor_cb_t descriptor=nullptr;
bool mounted=false,suspended=false,syncFailed=false;
uint32_t syncs=0;
uint32_t invalidLunYields=0;
uint8_t sense=0,asc=0,ascq=0;
std::array<uint8_t,16*512> disk{};
void begin(){
  assert(core.begin({nullptr,
    [](void*,uint32_t lba,uint8_t* out){std::memcpy(out,disk.data()+lba*512,512);return true;},
    [](void*,uint32_t lba,const uint8_t* in){std::memcpy(disk.data()+lba*512,in,512);return true;},
    [](void*){++syncs;return !syncFailed;}},16));
}
}
int tinyusb_enable_interface(uint8_t type,uint16_t length,tinyusb_descriptor_cb_t callback){assert(type==USB_INTERFACE_MSC&&length==23);descriptor=callback;return ESP_OK;}
uint8_t tinyusb_add_string_descriptor(const char* label){assert(!std::strcmp(label,"PaperDIY SD"));return 4;}
uint8_t tinyusb_get_free_duplex_endpoint(){return 1;}
bool tud_msc_set_sense(uint8_t,uint8_t key,uint8_t code,uint8_t qualifier){sense=key;asc=code;ascq=qualifier;return true;}
bool tud_mounted(){return mounted;}
bool tud_suspended(){return suspended;}
void vTaskDelay(uint32_t ticks){assert(ticks==1);++invalidLunYields;}
namespace usb_storage {
bool hostOwnsCard(){return core.owns();}
namespace transport_detail {
bool ready(){return core.ready();}
uint32_t sectors(){return core.ready()?core.status().sectors:0;}
int32_t read(uint32_t lba,uint32_t offset,void* bytes,uint32_t length){return core.read(lba,offset,bytes,length);}
int32_t write(uint32_t lba,uint32_t offset,const void* bytes,uint32_t length){return core.write(lba,offset,bytes,length);}
bool startStop(uint8_t power,bool start,bool eject){return core.startStop(power,start,eject);}
bool synchronize(uint32_t lba,uint16_t count){return core.synchronize(lba,count);}
bool preventRemoval(uint8_t prevent){return core.preventRemoval(prevent);}
Error lastError(){return core.status().error;}
}
}
extern "C" {
uint8_t tud_msc_get_maxlun_cb();
void tud_msc_inquiry_cb(uint8_t,uint8_t*,uint8_t*,uint8_t*);
bool tud_msc_test_unit_ready_cb(uint8_t);
void tud_msc_capacity_cb(uint8_t,uint32_t*,uint16_t*);
int32_t tud_msc_read10_cb(uint8_t,uint32_t,uint32_t,void*,uint32_t);
int32_t tud_msc_write10_cb(uint8_t,uint32_t,uint32_t,uint8_t*,uint32_t);
bool tud_msc_start_stop_cb(uint8_t,uint8_t,bool,bool);
int32_t tud_msc_scsi_cb(uint8_t,const uint8_t*,void*,uint16_t);
}
int main(){
  usb_transport::setStorageOwned(true);assert(!Serial.reboot);
  usb_transport::setStorageOwned(false);assert(Serial.reboot);
  assert(usb_transport::storageSupported());
  uint8_t bytes[64]={},interface=2;
  assert(descriptor&&descriptor(bytes,&interface)==23&&interface==3);
  assert(bytes[2]==2&&bytes[11]==1&&bytes[18]==0x81);
  assert(tud_msc_get_maxlun_cb()==0);
  uint8_t vendor[8],product[16],revision[4];
  tud_msc_inquiry_cb(0,vendor,product,revision);
  assert(!std::memcmp(vendor,"PaperDIY",8));
  uint32_t count=123;uint16_t size=123;
  tud_msc_capacity_cb(0,&count,&size);assert(!count&&!size);
  assert(!tud_msc_test_unit_ready_cb(0)&&sense==2&&asc==0x3a);
  assert(!usb_transport::blocksSleep());
  begin();assert(usb_transport::blocksSleep());
  assert(tud_msc_test_unit_ready_cb(0));
  tud_msc_capacity_cb(0,&count,&size);assert(count==16&&size==512);
  assert(tud_msc_read10_cb(1,0,0,bytes,1)<0&&sense==5&&asc==0x25);
  assert(tud_msc_write10_cb(1,0,0,bytes,1)<0&&sense==5&&asc==0x25);
  assert(invalidLunYields==2);
  bytes[0]=0x7a;assert(tud_msc_write10_cb(0,1,511,bytes,1)==1);
  bytes[0]=0;assert(tud_msc_read10_cb(0,1,511,bytes,1)==1&&bytes[0]==0x7a);
  assert(tud_msc_read10_cb(0,16,0,bytes,1)<0&&sense==5&&asc==0x21);
  uint8_t command[16]={0x35};
  assert(tud_msc_scsi_cb(0,command,nullptr,0)==0&&syncs==1&&core.owns());
  command[1]=2;assert(tud_msc_scsi_cb(0,command,nullptr,0)<0&&syncs==1&&sense==5&&asc==0x24);
  command[1]=0;command[5]=16;
  assert(tud_msc_scsi_cb(0,command,nullptr,0)<0&&syncs==1&&sense==5&&asc==0x21);
  command[5]=15;command[8]=2;
  assert(tud_msc_scsi_cb(0,command,nullptr,0)<0&&syncs==1);
  command[8]=1;assert(tud_msc_scsi_cb(0,command,nullptr,0)==0&&syncs==2);
  std::memset(command,0,16);command[0]=0x1e;command[4]=1;
  assert(tud_msc_scsi_cb(0,command,nullptr,0)==0);
  assert(!tud_msc_start_stop_cb(0,0,false,true)&&sense==5&&asc==0x53&&ascq==2);
  command[4]=2;assert(tud_msc_scsi_cb(0,command,nullptr,0)<0);
  command[4]=0;assert(tud_msc_scsi_cb(0,command,nullptr,0)==0);
  assert(!core.requestExit());
  mounted=true;suspended=true;assert(usb_transport::blocksSleep());
  mounted=false;suspended=false;assert(usb_transport::blocksSleep()&&core.owns());
  syncFailed=true;
  assert(!tud_msc_start_stop_cb(0,0,false,true)&&sense==3&&asc==0x0c&&core.owns());
  syncFailed=false;
  assert(tud_msc_start_stop_cb(0,0,false,true));assert(!usb_transport::blocksSleep());
  assert(!tud_msc_test_unit_ready_cb(0));
  assert(tud_msc_read10_cb(0,0,0,bytes,1)<0);
  std::memset(command,0,16);command[0]=0x91;
  assert(tud_msc_scsi_cb(0,command,nullptr,0)<0&&sense==5&&asc==0x20);
  std::puts("usb_transport callback tests passed");
}
