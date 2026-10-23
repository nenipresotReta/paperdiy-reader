#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Independent, versioned NVS record. Never reuse Wi-Fi or bookmark keys.
namespace reader_settings {
struct Config {
  bool punctuation=true,hyphenation=true,justify=true;
  uint8_t spacing=4,preset=1,refresh=1,ink=50,art=0;
  uint16_t doubleMs=200;
};
inline Config& current(){static Config value;return value;}
inline bool valid(const Config& c){
  return (c.spacing==0||c.spacing==2||c.spacing==4)&&c.preset<=2&&c.refresh<=2&&
    c.ink<=100&&c.art<=3&&c.doubleMs>=100&&c.doubleMs<=400;
}
struct Geometry {int pixels,lineHeight,left,right;};
inline Geometry geometry(){
  switch(current().preset){case 0:return {24,34,18,516};case 2:return {28,46,36,498};default:return {28,40,24,510};}
}
inline uint32_t crc(const uint8_t* p,size_t n){
  uint32_t v=~0u;while(n--){v^=*p++;for(int i=0;i<8;++i)v=(v>>1)^(0xedb88320u&uint32_t(-int32_t(v&1)));}return ~v;
}
constexpr size_t WIRE_SIZE=20;
inline void encode(const Config& c,uint8_t (&out)[WIRE_SIZE]){
  memset(out,0,sizeof(out));out[0]='P';out[1]='S';out[2]=1;
  out[3]=(c.punctuation?1:0)|(c.hyphenation?2:0)|(c.justify?4:0);
  out[4]=c.spacing;out[5]=c.preset;out[6]=c.refresh;out[7]=c.ink;out[8]=c.art;
  out[9]=c.doubleMs&255;out[10]=c.doubleMs>>8;
  uint32_t sum=crc(out,16);for(int i=0;i<4;++i)out[16+i]=uint8_t(sum>>(8*i));
}
inline bool decode(const uint8_t* in,size_t size,Config& c){
  if(size!=WIRE_SIZE||in[0]!='P'||in[1]!='S'||in[2]!=1||(in[3]&~7u))return false;
  for(int i=11;i<16;++i)if(in[i])return false;
  uint32_t sum=0;for(int i=0;i<4;++i)sum|=uint32_t(in[16+i])<<(8*i);
  if(crc(in,16)!=sum)return false;
  Config next;next.punctuation=in[3]&1;next.hyphenation=in[3]&2;next.justify=in[3]&4;
  next.spacing=in[4];next.preset=in[5];next.refresh=in[6];next.ink=in[7];next.art=in[8];next.doubleMs=in[9]|uint16_t(in[10])<<8;
  if(!valid(next))return false;c=next;return true;
}
template<class Store> bool load(Store& store){
  uint8_t data[WIRE_SIZE];Config loaded;
  if(store.getBytesLength("readerCfgV1")!=WIRE_SIZE||store.getBytes("readerCfgV1",data,WIRE_SIZE)!=WIRE_SIZE||!decode(data,WIRE_SIZE,loaded))return false;
  current()=loaded;return true;
}
template<class Store> bool save(Store& store,const Config& next){
  if(!valid(next))return false;
  uint8_t data[WIRE_SIZE],old[WIRE_SIZE];encode(next,data);encode(current(),old);
  if(!memcmp(data,old,WIRE_SIZE))return true;
  if(store.putBytes("readerCfgV1",data,WIRE_SIZE)!=WIRE_SIZE)return false;
  current()=next;return true;
}
// Only geometry-affecting options participate in cache identity.
inline uint32_t layoutBits(){const auto& c=current();return (c.spacing==2?16u:0u)|(uint32_t(c.preset)<<5)|(c.hyphenation?0u:128u)|(c.justify?0u:256u);}
inline bool keepInk(int x,int y){
  // Default 50% exactly preserves the historical checkerboard.
  static constexpr uint8_t rank[4][4]={{8,0,10,2},{4,12,6,14},{11,3,9,1},{7,15,5,13}};
  return unsigned(rank[y&3][x&3])*100u<unsigned(current().ink)*16u;
}
}
