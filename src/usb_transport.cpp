#include "usb_transport.h"
#include "usb_storage.h"
#ifdef ARDUINO
#include <Arduino.h>
#endif

#if PAPERDIY_EXPERIMENTAL_USB_MSC && defined(ARDUINO) && !ARDUINO_USB_MODE && CONFIG_TINYUSB_MSC_ENABLED
#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <freertos/task.h>

namespace {
// Register before Arduino's app_main calls USB.begin(). Media stays absent
// until explicit U entry. Do not instantiate SDK USBMSC: its strong callbacks
// do not implement SYNCHRONIZE CACHE or enforce PREVENT/ALLOW removal.
bool descriptorReady=false;
uint16_t descriptor(uint8_t* destination,uint8_t* interface){
  const uint8_t string=tinyusb_add_string_descriptor("PaperDIY SD");
  const uint8_t endpoint=tinyusb_get_free_duplex_endpoint();
  if(!endpoint)return 0;
  const uint8_t bytes[]={TUD_MSC_DESCRIPTOR(*interface,string,endpoint,uint8_t(0x80|endpoint),64)};
  ++*interface;std::memcpy(destination,bytes,sizeof(bytes));return sizeof(bytes);
}
struct Registration {
  Registration(){descriptorReady=tinyusb_enable_interface(USB_INTERFACE_MSC,TUD_MSC_DESC_LEN,descriptor)==ESP_OK;}
} registration;
bool validLun(uint8_t lun){if(!lun)return true;tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x25,0);return false;}
void failureSense(uint8_t lun){
  using usb_storage::Error;
  switch(usb_storage::transport_detail::lastError()){
    case Error::Range:tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x21,0);break;
    case Error::Read:tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,0x11,0);break;
    case Error::Write:case Error::Sync:tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,0x0c,2);break;
    case Error::Prevented:tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x53,2);break;
    case Error::Unsupported:tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x24,0);break;
    default:tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);break;
  }
}
}
namespace usb_transport {
Connection connection(){return {tud_mounted(),tud_suspended()};}
bool hostConnected(){return connection().configured;}
bool blocksSleep(){const auto c=connection();return c.configured||c.suspended||usb_storage::hostOwnsCard();}
bool storageSupported(){return descriptorReady;}
void setStorageOwned(bool owned){Serial.enableReboot(!owned);}
}
extern "C" {
uint8_t tud_msc_get_maxlun_cb(){return 0;}
void tud_msc_inquiry_cb(uint8_t,uint8_t vendor[8],uint8_t product[16],uint8_t revision[4]){
  std::memcpy(vendor,"PaperDIY",8);
#ifdef PAPERDIY_USB_RAM_DISK
  std::memcpy(product,"RAM Test Disk   ",16);
#else
  std::memcpy(product,"SD Book Transfer",16);
#endif
  std::memcpy(revision,"1.0 ",4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun){
  if(validLun(lun)&&usb_storage::transport_detail::ready())return true;
  if(!lun)tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);return false;
}
void tud_msc_capacity_cb(uint8_t lun,uint32_t* count,uint16_t* size){
  *count=validLun(lun)?usb_storage::transport_detail::sectors():0;
  *size=*count?usb_storage::SECTOR_SIZE:0;
}
bool tud_msc_is_writable_cb(uint8_t lun){return validLun(lun)&&usb_storage::transport_detail::ready();}
int32_t tud_msc_read10_cb(uint8_t lun,uint32_t lba,uint32_t offset,void* bytes,uint32_t length){
  if(!validLun(lun)){vTaskDelay(1);return -1;}
  const auto n=usb_storage::transport_detail::read(lba,offset,bytes,length);
  if(n<0)failureSense(lun);return n;
}
int32_t tud_msc_write10_cb(uint8_t lun,uint32_t lba,uint32_t offset,uint8_t* bytes,uint32_t length){
  if(!validLun(lun)){vTaskDelay(1);return -1;}
  const auto n=usb_storage::transport_detail::write(lba,offset,bytes,length);
  if(n<0)failureSense(lun);return n;
}
bool tud_msc_start_stop_cb(uint8_t lun,uint8_t power,bool start,bool eject){
  if(!validLun(lun))return false;
  const bool ok=usb_storage::transport_detail::startStop(power,start,eject);
  if(!ok)failureSense(lun);return ok;
}
int32_t tud_msc_scsi_cb(uint8_t lun,const uint8_t command[16],void*,uint16_t){
  if(!validLun(lun))return -1;
  bool ok=false;
  switch(command[0]){
    case 0x35: // SYNCHRONIZE CACHE(10), synchronous, whole-device barrier.
      // IMMED is deliberately unsupported: never acknowledge deferred flush.
      if(command[1]||command[6]||command[9]){
        tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x24,0);return -1;
      }
      ok=usb_storage::transport_detail::synchronize(
        uint32_t(command[2])<<24|uint32_t(command[3])<<16|uint32_t(command[4])<<8|command[5],
        uint16_t(uint16_t(command[7])<<8|command[8]));break;
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
      if(command[1]||command[2]||command[3]||command[5]){
        tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x24,0);return -1;
      }
      ok=usb_storage::transport_detail::preventRemoval(command[4]);break;
    default:tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x20,0);return -1;
  }
  if(!ok){failureSense(lun);return -1;}return 0;
}
}
#elif defined(ARDUINO) && !ARDUINO_USB_MODE
#include <USB.h>
#include <esp32-hal-tinyusb.h>
namespace usb_transport {
Connection connection(){return {tud_mounted(),tud_suspended()};}
bool hostConnected(){return connection().configured;}
bool blocksSleep(){const auto c=connection();return c.configured||c.suspended;}
bool storageSupported(){return false;}
void setStorageOwned(bool){}
}
#elif defined(ARDUINO) && ARDUINO_USB_MODE
#include <HWCDC.h>
namespace usb_transport {
Connection connection(){return {HWCDC::isPlugged(),false};}
bool hostConnected(){return HWCDC::isPlugged();}
bool blocksSleep(){return hostConnected()||usb_storage::hostOwnsCard();}
bool storageSupported(){return false;}
void setStorageOwned(bool){}
}
#else
namespace usb_transport {
Connection connection(){return {};}
bool hostConnected(){return false;}
bool blocksSleep(){return usb_storage::hostOwnsCard();}
bool storageSupported(){return false;}
void setStorageOwned(bool){}
}
#endif
