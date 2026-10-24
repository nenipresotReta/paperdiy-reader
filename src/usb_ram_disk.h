#pragma once
#include "usb_storage_core.h"

namespace usb_storage {
// Synthetic, disposable FAT12 superfloppy; never backed by a card or host file.
constexpr uint32_t RAM_SECTORS=128;
inline bool initializeRamDisk(uint8_t* disk,size_t bytes){
  if(!disk||bytes!=size_t(RAM_SECTORS)*SECTOR_SIZE)return false;
  std::memset(disk,0,bytes);
  disk[0]=0xeb;disk[1]=0x3c;disk[2]=0x90;
  std::memcpy(disk+3,"MSDOS5.0",8);
  auto le16=[&](size_t p,uint16_t v){disk[p]=uint8_t(v);disk[p+1]=uint8_t(v>>8);};
  le16(11,SECTOR_SIZE);disk[13]=1;le16(14,1);disk[16]=2;
  le16(17,32);le16(19,RAM_SECTORS);disk[21]=0xf8;le16(22,1);
  le16(24,1);le16(26,1);disk[36]=0x80;disk[38]=0x29;
  disk[39]=0x50;disk[40]=0x44;disk[41]=0x49;disk[42]=0x59;
  std::memcpy(disk+43,"PAPERDIYUSB",11);std::memcpy(disk+54,"FAT12   ",8);
  disk[510]=0x55;disk[511]=0xaa;
  for(size_t p : {size_t(512),size_t(1024)}){
    disk[p]=0xf8;disk[p+1]=0xff;disk[p+2]=0xff;
    disk[p+3]=0xff;disk[p+4]=0x0f; // cluster 2 EOF
  }
  constexpr char message[]="PaperDIY disposable USB test disk. No SD card data is exposed.\r\n";
  auto* root=disk+3*SECTOR_SIZE;
  std::memcpy(root,"README  TXT",11);root[11]=0x20;root[26]=2;
  root[28]=uint8_t(sizeof(message)-1);
  std::memcpy(disk+5*SECTOR_SIZE,message,sizeof(message)-1);
  return true;
}
}
