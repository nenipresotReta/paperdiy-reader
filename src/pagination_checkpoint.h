#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// A partial canonical prefix is a different product from a complete page
// cache. Two fixed slots are sufficient; a torn newer slot leaves the other
// readable. This internal SD format, like the complete cache, is little-endian.
namespace pagination_checkpoint {
constexpr uint32_t MAGIC=0x31435050u,VERSION=1;
constexpr size_t MAX_PAGES=65536,IO_BYTES=8192;
constexpr size_t FIRST_PAGES=32,PAGE_INTERVAL=128;
constexpr uint32_t WRITE_INTERVAL_MS=5000;
struct Identity {
  uint32_t layout=0,textRevision=0,textBytes=0;
  uint8_t source[32]={},dependencies[32]={},content[32]={};
};
struct Header {
  uint32_t magic=MAGIC,version=VERSION,headerBytes=sizeof(Header);
  uint32_t generation=0,pages=0,cursor=0,reserved=0;
  Identity identity;
  uint8_t digest[32]={};
};
static_assert(sizeof(Identity)==108,"checkpoint identity layout");
static_assert(sizeof(Header)==168,"checkpoint header layout");
constexpr size_t MAX_FILE_BYTES=sizeof(Header)+MAX_PAGES*sizeof(uint32_t);
constexpr size_t MAX_DISK_BYTES=2*MAX_FILE_BYTES;
inline bool same(const Identity& a,const Identity& b){return !std::memcmp(&a,&b,sizeof(a));}
inline bool validHeader(const Header& h,uint64_t fileBytes,const Identity& expected){
  return h.magic==MAGIC&&h.version==VERSION&&h.headerBytes==sizeof(Header)&&!h.reserved&&
    h.generation&&h.pages&&h.pages<=MAX_PAGES&&h.identity.textBytes&&
    h.cursor&&h.cursor<h.identity.textBytes&&same(h.identity,expected)&&
    fileBytes==sizeof(Header)+uint64_t(h.pages)*sizeof(uint32_t);
}
inline bool validPrefix(const uint32_t* pages,size_t count,uint32_t cursor,size_t length){
  if(!pages||!count||count>MAX_PAGES||!length||length>UINT32_MAX||
     !cursor||cursor>=length||pages[0]||pages[count-1]>=cursor)return false;
  for(size_t i=1;i<count;++i)if(pages[i]<=pages[i-1])return false;
  return true;
}
} // namespace pagination_checkpoint
