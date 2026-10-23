#pragma once
#include "reader_settings.h"
#include <memory>
#include <new>
namespace shutdown_art {
constexpr size_t HEADER=16,MAX_BYTES=16+68*960;
inline const char* path(uint8_t slot){return slot==1?"/.paperdiy/shutdown-1.bin":"/.paperdiy/shutdown-2.bin";}
inline uint16_t word(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline bool valid(const uint8_t* bytes,size_t size){
  if(size<HEADER||size>MAX_BYTES||memcmp(bytes,"PDI1",4))return false;
  const unsigned w=word(bytes+4),h=word(bytes+6);
  if(!w||w>540||!h||h>960||size!=HEADER+((w+7)/8)*h)return false;
  for(unsigned i=8;i<12;++i)if(bytes[i])return false;
  uint32_t sum=0;for(int i=0;i<4;++i)sum|=uint32_t(bytes[12+i])<<(8*i);
  return sum==reader_settings::crc(bytes+HEADER,size-HEADER);
}
template<class File> std::unique_ptr<uint8_t[]> read(File& file){
  if(!file||file.size()<HEADER||file.size()>MAX_BYTES)return {};
  const size_t size=file.size();std::unique_ptr<uint8_t[]> data(new(std::nothrow) uint8_t[size]);
  if(!data||file.read(data.get(),size)!=size||!valid(data.get(),size))return {};
  return data;
}
}
// Returns false if an uploaded image is unavailable; the caller may use its
// compiled fallback. Never draws a partial or unvalidated image.
bool drawStoredShutdownArt();
