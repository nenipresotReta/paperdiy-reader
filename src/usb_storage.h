#pragma once
#include "usb_features.h"
#include "usb_storage_core.h"
#include "usb_diagnostics.h"

namespace usb_storage {
// Application task only. BEFORE begin: save progress, stop networking, close
// every file/cache, revoke source leases and SD.end(). AFTER release: remount
// locally and rescan. Never call SD.begin while hostOwnsCard() is true.
// RAM test builds never initialize/read/write the physical SD block device.
bool available();
bool ramTestMode();
bool begin(uint32_t spiFrequencyHz);
void poll();
// False requires the application to retain U, block shutdown/mode changes and
// show "Eject on computer". Disconnect/reset/suspend/idle does not grant release.
bool requestExit();
Status status();
bool hostOwnsCard();
// Ejected is observable until the app acknowledges; acknowledgment returns to
// Inactive, only after poll has removed the raw driver. Then remount locally.
bool finish();

namespace transport_detail {
bool ready();
uint32_t sectors();
int32_t read(uint32_t lba,uint32_t offset,void* buffer,uint32_t length);
int32_t write(uint32_t lba,uint32_t offset,const void* buffer,uint32_t length);
bool startStop(uint8_t power,bool start,bool loadEject);
bool synchronize(uint32_t lba=0,uint16_t count=0);
bool preventRemoval(uint8_t prevent);
Error lastError();
}
}
