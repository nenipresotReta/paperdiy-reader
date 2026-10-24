#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "cache_format.h"

// Optional acceleration index for an already complete cache. The complete
// generation remains independently readable, and a missing/stale index falls
// back to its ordinary whole-file verification. No source proof persists here.
namespace content_manifest {
constexpr uint32_t MAGIC=0x31424350,VERSION=1,BLOCK_BYTES=8192,MAX_BLOCKS=512;
struct Header {
  uint32_t magic=MAGIC,version=VERSION,textBytes=0,prefixBytes=0,blockBytes=BLOCK_BYTES,blocks=0;
  uint8_t cacheDigest[32]={},prefixDigest[32]={},digest[32]={};
};
static_assert(sizeof(Header)==120,"content manifest header");
constexpr size_t MAX_BYTES=sizeof(Header)+MAX_BLOCKS*32;
inline size_t blocks(size_t bytes){return (bytes+BLOCK_BYTES-1)/BLOCK_BYTES;}
inline bool valid(const Header& h,size_t bytes,const reader_cache::Header& cache){
  return h.magic==MAGIC&&h.version==VERSION&&h.blockBytes==BLOCK_BYTES&&
    h.textBytes==cache.textBytes&&h.textBytes&&h.blocks==blocks(h.textBytes)&&h.blocks<=MAX_BLOCKS&&
    h.prefixBytes==sizeof(cache)+uint64_t(cache.metaBytes)+uint64_t(cache.pages)*4&&
    bytes==sizeof(h)+size_t(h.blocks)*32&&!std::memcmp(h.cacheDigest,cache.digest,32);
}
} // namespace content_manifest
