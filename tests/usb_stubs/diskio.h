#pragma once
#include <cstdint>
constexpr uint8_t STA_NOINIT=1,STA_NODISK=2,CTRL_SYNC=0;
constexpr int RES_OK=0,RES_ERROR=1;
uint8_t disk_initialize(uint8_t);
int disk_ioctl(uint8_t,uint8_t,void*);
int disk_read(uint8_t,uint8_t*,uint32_t,uint32_t);
int disk_write(uint8_t,const uint8_t*,uint32_t,uint32_t);
