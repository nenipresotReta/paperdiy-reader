#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace extraction_checkpoint {
constexpr uint32_t MAGIC=0x31455850u; // PXE1, explicit little endian.
constexpr uint32_t VERSION=1,EXTRACTOR=2;
constexpr size_t HEADER_BYTES=192,MAX_METADATA=1024*1024,IO_BYTES=8192;
constexpr uint32_t COMPLETE=1,BODY=2,UNSAFE_LANGUAGE=4;
inline uint32_t get(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline void put(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
struct Header {
 uint32_t generation=0,textRevision=0,semanticRevision=0,flags=0,textBytes=0,cursor=0,chapters=0;
 uint32_t language=0,collectorLanguage=0,collectorBytes=0,deps=0,depBytes=0;
 uint8_t source[32]={},plan[32]={},textDigest[32]={},digest[32]={};
};
inline void encode(const Header& h,uint8_t* p){
 const uint32_t f[16]={MAGIC,VERSION,uint32_t(HEADER_BYTES+h.collectorBytes+h.depBytes),h.generation,
 h.textRevision,h.semanticRevision,EXTRACTOR,h.flags,h.textBytes,h.cursor,h.chapters,h.language,
 h.collectorLanguage,h.collectorBytes,h.deps,h.depBytes};
 for(unsigned i=0;i<16;++i)put(p+i*4,f[i]);
 memcpy(p+64,h.source,32);memcpy(p+96,h.plan,32);memcpy(p+128,h.textDigest,32);memcpy(p+160,h.digest,32);
}
inline bool decode(const uint8_t* p,size_t fileBytes,size_t maxText,uint32_t textRevision,uint32_t semantics,
                   const uint8_t* source,const uint8_t* plan,Header& h){
 if(get(p)!=MAGIC||get(p+4)!=VERSION||get(p+8)!=fileBytes||fileBytes<HEADER_BYTES||fileBytes>HEADER_BYTES+MAX_METADATA||
    get(p+16)!=textRevision||get(p+20)!=semantics||get(p+24)!=EXTRACTOR||memcmp(p+64,source,32)||memcmp(p+96,plan,32))return false;
 h.generation=get(p+12);h.textRevision=get(p+16);h.semanticRevision=get(p+20);h.flags=get(p+28);
 h.textBytes=get(p+32);h.cursor=get(p+36);h.chapters=get(p+40);h.language=get(p+44);
 h.collectorLanguage=get(p+48);h.collectorBytes=get(p+52);h.deps=get(p+56);h.depBytes=get(p+60);
 if(!h.generation||h.generation==UINT32_MAX||(h.flags&~7u)||!h.textBytes||h.textBytes>maxText||!h.chapters||h.chapters>2048||h.cursor>h.chapters||
    h.language>2||h.collectorLanguage>=32||!h.collectorBytes||!h.deps||h.deps>4096||
    h.depBytes<h.deps*44u||uint64_t(h.collectorBytes)+h.depBytes!=fileBytes-HEADER_BYTES||
    ((h.flags&COMPLETE)?(h.cursor!=h.chapters||!(h.flags&BODY)):(h.cursor>=h.chapters)))return false;
 memcpy(h.source,p+64,32);memcpy(h.plan,p+96,32);memcpy(h.textDigest,p+128,32);memcpy(h.digest,p+160,32);return true;
}
}
