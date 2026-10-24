#pragma once
#include <cstdint>
constexpr int ESP_OK=0;
constexpr uint8_t USB_INTERFACE_MSC=0;
constexpr uint8_t SCSI_SENSE_NOT_READY=2,SCSI_SENSE_MEDIUM_ERROR=3,SCSI_SENSE_ILLEGAL_REQUEST=5;
constexpr uint8_t SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL=0x1e;
constexpr uint16_t TUD_MSC_DESC_LEN=23;
using tinyusb_descriptor_cb_t=uint16_t (*)(uint8_t*,uint8_t*);
int tinyusb_enable_interface(uint8_t,uint16_t,tinyusb_descriptor_cb_t);
uint8_t tinyusb_add_string_descriptor(const char*);
uint8_t tinyusb_get_free_duplex_endpoint();
bool tud_msc_set_sense(uint8_t,uint8_t,uint8_t,uint8_t);
bool tud_mounted();
bool tud_suspended();
#define TUD_MSC_DESCRIPTOR(i,s,out,in,packet) 9,4,i,0,2,8,6,0x50,s,7,5,out,2,uint8_t(packet),0,0,7,5,in,2,uint8_t(packet),0,0
