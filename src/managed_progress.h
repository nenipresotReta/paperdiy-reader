#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "bookmark_store.h"

// The rolling Push document owns two fixed progress records. Its revision
// identity lives inside one atomic NVS blob, never in a growing key namespace.
namespace managed_progress {
constexpr size_t MAX_BYTES=16*1024,RECORD_BYTES=48;
inline int slot(const char* path){
  if(!path)return -1;
  if(!std::strcmp(path,"/.paperdiy/lan/push0.md"))return 0;
  if(!std::strcmp(path,"/.paperdiy/lan/push1.md"))return 1;
  return -1;
}
inline bool isPushPath(const char* path){return slot(path)>=0;}
inline const char* key(int index){return index==0?"pushRead0":index==1?"pushRead1":nullptr;}
inline uint32_t word(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline void word(uint8_t* p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
inline uint32_t checksum(const uint8_t* p,size_t n){
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<n;++i){crc^=p[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
  return ~crc;
}
inline bool valid(const uint8_t* data){
  return !std::memcmp(data,"PRP1",4)&&word(data+4)&&word(data+4)<=MAX_BYTES&&
    word(data+8)<word(data+4)&&word(data+44)==checksum(data,44);
}
template<class Preferences>
bool read(Preferences& prefs,int index,const uint8_t digest[32],size_t bytes,uint32_t& offset){
  const char* name=key(index);if(!name||!bytes||bytes>MAX_BYTES)return false;
  offset=0;if(!prefs.isKey(name))return true;
  uint8_t data[RECORD_BYTES];
  if(prefs.getBytesLength(name)!=sizeof(data)||prefs.getBytes(name,data,sizeof(data))!=sizeof(data)||!valid(data))return false;
  if(word(data+4)==bytes&&!std::memcmp(data+12,digest,32))offset=word(data+8);
  return true;
}
template<class Preferences>
bookmarks::SaveResult save(Preferences& prefs,bool available,int index,const uint8_t digest[32],
                           size_t bytes,uint32_t offset,const char* path,size_t pathLength){
  if(!available)return bookmarks::SaveResult::Unavailable;
  const char* name=key(index);
  if(!name||slot(path)!=index||!bytes||bytes>MAX_BYTES||offset>=bytes)return bookmarks::SaveResult::PositionFailed;
  uint8_t data[RECORD_BYTES]={},previous[RECORD_BYTES];std::memcpy(data,"PRP1",4);
  word(data+4,uint32_t(bytes));word(data+8,offset);std::memcpy(data+12,digest,32);word(data+44,checksum(data,44));
  const bool unchanged=prefs.getBytesLength(name)==sizeof(previous)&&
    prefs.getBytes(name,previous,sizeof(previous))==sizeof(previous)&&!std::memcmp(data,previous,sizeof(data));
  if(!unchanged&&(prefs.putBytes(name,data,sizeof(data))!=sizeof(data)||
      prefs.getBytesLength(name)!=sizeof(previous)||prefs.getBytes(name,previous,sizeof(previous))!=sizeof(previous)||
      std::memcmp(data,previous,sizeof(data))))return bookmarks::SaveResult::PositionFailed;
  if(prefs.getString("sdLast","")!=path&&prefs.putString("sdLast",path)!=pathLength)return bookmarks::SaveResult::ResumeFailed;
  return bookmarks::SaveResult::Saved;
}
}
