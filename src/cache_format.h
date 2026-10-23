#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "reader_settings.h"
namespace reader_cache {
constexpr uint32_t MAGIC=0x31434450, VERSION=2;
// Low bits encode independent P/S settings and the text mode. Bump the
// family for any other geometry/extraction change; old text remains readable
// for bookmark migration, but its page offsets must never be a cache hit.
constexpr uint32_t LAYOUT_BASE=0x20262000;
constexpr uint32_t LAYOUT=LAYOUT_BASE|35,LAYOUT_NO_HYPHENS=LAYOUT_BASE|39,LAYOUT_MARKDOWN=LAYOUT_BASE|43;
inline uint32_t layoutFor(bool englishHyphens,bool markdown=false,uint8_t style=3) {
  return LAYOUT_BASE|reader_settings::layoutBits()|(markdown?8u:englishHyphens?0u:4u)|(style&3u);
}
inline bool currentLayout(uint32_t value) {
  return (value&~511u)==LAYOUT_BASE&&(value&12u)!=12u&&((value>>5)&3u)<=2;
}
inline bool markdownLayout(uint32_t value){return currentLayout(value)&&(value&12u)==8u;}
inline bool englishHyphens(uint32_t value){return currentLayout(value)&&(value&12u)!=4u;}
inline bool matchesLayout(uint32_t value,bool markdown,uint8_t style){
  return currentLayout(value)&&value==layoutFor(englishHyphens(value),markdown,style);
}
constexpr uint32_t MAX_DEPS=4096,MAX_META=512*1024,MAX_PAGES=65536;
constexpr uint32_t DEP_BYTES=12+32, LEGACY_DEP_BYTES=12;
struct Header {
  uint32_t magic,version,layout,directory,textBytes,pages,deps,metaBytes;
  uint8_t source[32];char bookmark[16];uint8_t digest[32];
};
static_assert(sizeof(Header)==112,"cache header size");
inline bool validEnvelope(const Header& h,uint64_t fileSize,size_t capacity,const uint8_t source[32],bool directory){
  if(h.magic!=MAGIC||h.directory!=uint32_t(directory)||memcmp(h.source,source,32))return false;
  if(!h.textBytes||h.textBytes>capacity||!h.pages||h.pages>MAX_PAGES||h.pages>h.textBytes||!h.deps||h.deps>MAX_DEPS||h.metaBytes>MAX_META)return false;
  if(!directory&&h.deps!=1)return false;
  if((h.bookmark[0]!='b'&&h.bookmark[0]!='c')||h.bookmark[15]!=0)return false;
  for(int i=1;i<15;++i)if(!((h.bookmark[i]>='0'&&h.bookmark[i]<='9')||(h.bookmark[i]>='a'&&h.bookmark[i]<='f')))return false;
  return fileSize==sizeof(Header)+uint64_t(h.metaBytes)+uint64_t(h.pages)*4+h.textBytes;
}
inline bool validHeader(const Header& h,uint64_t fileSize,size_t capacity,const uint8_t source[32],bool directory){
  return h.version==VERSION&&currentLayout(h.layout)&&h.bookmark[0]=='c'&&h.metaBytes>=uint64_t(h.deps)*DEP_BYTES&&
    validEnvelope(h,fileSize,capacity,source,directory);
}
inline bool validLegacyHeader(const Header& h,uint64_t fileSize,size_t capacity,const uint8_t source[32],bool directory){
  // Layout changed several times on deployed devices. Migration needs the v1
  // envelope and text, not its old font/page geometry.
  return h.version==1&&h.bookmark[0]=='b'&&
    h.metaBytes>=uint64_t(h.deps)*LEGACY_DEP_BYTES&&validEnvelope(h,fileSize,capacity,source,directory);
}
inline bool validMigrationHeader(const Header& h,uint64_t size,size_t capacity,const uint8_t source[32],bool directory){
  return validLegacyHeader(h,size,capacity,source,directory) ||
    (h.version==2 && h.bookmark[0]=='c' &&
     h.metaBytes>=uint64_t(h.deps)*DEP_BYTES && validEnvelope(h,size,capacity,source,directory));
}
inline bool validOffsets(const uint32_t* offsets,size_t count,size_t bytes){
  if(!count||offsets[0]!=0)return false;
  for(size_t i=0;i<count;++i)if(offsets[i]>=bytes||(i&&offsets[i]<=offsets[i-1]))return false;
  return true;
}
inline bool validResource(const char* p,size_t n){
  if(n>1024||(n&&p[0]=='/'))return false;
  size_t start=0;
  for(size_t i=0;i<=n;++i){
    if(i<n&&(p[i]==0||p[i]=='\\'))return false;
    if(i==n||p[i]=='/'){if(i-start==2&&p[start]=='.'&&p[start+1]=='.')return false;start=i+1;}
  }
  return true;
}
}
