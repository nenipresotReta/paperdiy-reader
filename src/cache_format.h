#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace reader_cache {
constexpr uint32_t MAGIC=0x31434450, VERSION=2;
// Bump when font, geometry, wrapping, or EPUB text extraction changes.
constexpr uint32_t LAYOUT=0x20261026;
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
  return h.version==VERSION&&h.layout==LAYOUT&&h.bookmark[0]=='c'&&h.metaBytes>=uint64_t(h.deps)*DEP_BYTES&&
    validEnvelope(h,fileSize,capacity,source,directory);
}
inline bool validLegacyHeader(const Header& h,uint64_t fileSize,size_t capacity,const uint8_t source[32],bool directory){
  // Layout changed several times on deployed devices. Migration needs the v1
  // envelope and text, not its old font/page geometry.
  return h.version==1&&h.bookmark[0]=='b'&&
    h.metaBytes>=uint64_t(h.deps)*LEGACY_DEP_BYTES&&validEnvelope(h,fileSize,capacity,source,directory);
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
