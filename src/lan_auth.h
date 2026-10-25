#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "lan_nvs.h"
#ifdef ARDUINO
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/platform_util.h>
#endif

// Per-client verifiers occupy one bounded, atomic NVS value. Raw bearer tokens
// are returned once, over pinned TLS; neither logs nor this record retain them.
namespace lan_auth {
constexpr size_t CAPACITY=4, LABEL_BYTES=48;
constexpr size_t RECORD_BYTES=8+CAPACITY*(8+32+1+LABEL_BYTES)+4;
inline bool equal(const void* a,const void* b,size_t length){
#ifdef ARDUINO
  return mbedtls_ct_memcmp(a,b,length)==0;
#else
  const auto* x=static_cast<const uint8_t*>(a);const auto* y=static_cast<const uint8_t*>(b);
  volatile uint8_t difference=0;for(size_t i=0;i<length;++i)difference=uint8_t(difference|(x[i]^y[i]));return difference==0;
#endif
}
inline void erase(void* bytes,size_t length){
#ifdef ARDUINO
  mbedtls_platform_zeroize(bytes,length);
#else
  volatile uint8_t* out=static_cast<volatile uint8_t*>(bytes);for(size_t i=0;i<length;++i)out[i]=0;
#endif
}
template<size_t N>struct Secret{uint8_t bytes[N]{};~Secret(){erase(bytes,N);}};
inline bool labelValid(const std::string& text){
  if(text.empty()||text.size()>LABEL_BYTES)return false;
  for(size_t at=0;at<text.size();){uint8_t first=uint8_t(text[at++]);uint32_t cp=first;unsigned tail=0;uint32_t minimum=0;
    if(first<0x80){}else if(first>=0xc2&&first<=0xdf){tail=1;cp=first&31;minimum=128;}else if(first>=0xe0&&first<=0xef){tail=2;cp=first&15;minimum=2048;}else if(first>=0xf0&&first<=0xf4){tail=3;cp=first&7;minimum=65536;}else return false;
    if(tail>text.size()-at)return false;while(tail--){const uint8_t c=uint8_t(text[at++]);if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
    if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<32||(cp>=127&&cp<=159))return false;
  }return true;
}
inline bool hex(const std::string& text,size_t bytes){
  if(text.size()!=bytes*2)return false;
  for(char c:text)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
inline std::string encode(const uint8_t* bytes,size_t length){
  static constexpr char alphabet[]="0123456789abcdef";
  std::string out;out.reserve(length*2);
  for(size_t i=0;i<length;++i){out+=alphabet[bytes[i]>>4];out+=alphabet[bytes[i]&15];}return out;
}
inline bool decode(const std::string& text,uint8_t* bytes,size_t length){
  if(!hex(text,length))return false;
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(size_t i=0;i<length;++i)bytes[i]=uint8_t(digit(text[2*i])*16+digit(text[2*i+1]));return true;
}
inline uint32_t crc(const uint8_t* bytes,size_t length){
  uint32_t value=~0u;
  for(size_t i=0;i<length;++i){value^=bytes[i];for(unsigned bit=0;bit<8;++bit)value=(value>>1)^((0u-(value&1u))&0xedb88320u);}return ~value;
}
struct Client {uint8_t id[8]{},verifier[32]{};char label[LABEL_BYTES+1]{};bool used=false;};
class Registry {
 public:
  using Random=bool(*)(uint8_t*,size_t);
  using Hash=bool(*)(const uint8_t*,size_t,uint8_t[32]);
  template<class Preferences> bool load(Preferences& prefs){
    valid_=false;for(auto& client:clients_)client=Client{};
    const auto key=prefs.keyState(KEY);if(key==lan_nvs::KeyState::Error)return false;
    if(key==lan_nvs::KeyState::Absent){valid_=true;return true;}
    uint8_t data[RECORD_BYTES];
    if(prefs.getBytesLength(KEY)!=sizeof(data)||prefs.getBytes(KEY,data,sizeof(data))!=sizeof(data)||
       std::memcmp(data,"LAC1",4)||data[4]!=CAPACITY||data[5]||data[6]||data[7])return false;
    const uint32_t stored=uint32_t(data[RECORD_BYTES-4])|uint32_t(data[RECORD_BYTES-3])<<8|
      uint32_t(data[RECORD_BYTES-2])<<16|uint32_t(data[RECORD_BYTES-1])<<24;
    if(stored!=crc(data,sizeof(data)-4))return false;
    size_t offset=8;
    for(auto& client:clients_){
      std::memcpy(client.id,data+offset,8);offset+=8;
      std::memcpy(client.verifier,data+offset,32);offset+=32;
      const uint8_t size=data[offset++];if(size>LABEL_BYTES)return false;
      client.used=size!=0;
      for(size_t i=0;i<LABEL_BYTES;++i){
        const auto c=data[offset+i];if(i<size?(c<32||c==127):c!=0)return false;
        client.label[i]=char(c);
      }
      offset+=LABEL_BYTES;
      if(client.used&&!labelValid(client.label))return false;
      if(!client.used){for(auto c:client.id)if(c)return false;for(auto c:client.verifier)if(c)return false;}
    }
    for(size_t i=0;i<CAPACITY;++i)for(size_t j=0;j<i;++j)
      if(clients_[i].used&&clients_[j].used&&equal(clients_[i].id,clients_[j].id,8))return false;
    valid_=true;return true;
  }
  bool valid()const{return valid_;}
  size_t count()const{size_t n=0;for(const auto& client:clients_)n+=client.used;return n;}
  // Physical inventory exposes only the identifier and display label. Never
  // return token verifiers or make an invalid registry appear empty.
  bool clientInfo(size_t slot,std::string& id,std::string& label)const{
    id.clear();label.clear();
    if(!valid_||slot>=CAPACITY||!clients_[slot].used)return false;
    id=encode(clients_[slot].id,8);label=clients_[slot].label;return true;
  }
  bool authenticate(const std::string& authorization,Hash hash,std::string& id)const{
    id.clear();
    if(!valid_||authorization.size()!=7+16+1+64||authorization.compare(0,7,"Bearer ")||authorization[23]!=':')return false;
    uint8_t rawId[8];Secret<32> token,digest;
    if(!decode(authorization.substr(7,16),rawId,8)||!decode(authorization.substr(24),token.bytes,32)||!hash(token.bytes,32,digest.bytes))return false;
    bool accepted=false;
    for(const auto& client:clients_){const bool match=equal(client.id,rawId,8)&equal(client.verifier,digest.bytes,32);accepted|=bool(uint8_t(client.used)&uint8_t(match));}
    if(accepted)id=authorization.substr(7,16);return accepted;
  }
  template<class Preferences> bool add(Preferences& prefs,const std::string& label,Random random,Hash hash,std::string& id,std::string& token){
    id.clear();token.clear();
    if(!valid_||!labelValid(label)||count()==CAPACITY)return false;
    Registry next=*this;Client* slot=nullptr;
    for(auto& client:next.clients_)if(!client.used){slot=&client;break;}
    Secret<32> secret;if(!slot||!random(slot->id,8)||!random(secret.bytes,32)||!hash(secret.bytes,32,slot->verifier))return false;
    for(const auto& client:clients_)if(client.used&&equal(client.id,slot->id,8))return false;
    slot->used=true;std::memcpy(slot->label,label.data(),label.size());
    if(!next.save(prefs)){valid_=false;return false;}
    *this=next;id=encode(slot->id,8);token=encode(secret.bytes,32);return true;
  }
  // Explicit physical repair from the AP WebUI: replaces any record, even an
  // unreadable one, with a verified empty registry. Device identity is kept.
  template<class Preferences> bool reset(Preferences& prefs){
    Registry next;next.valid_=true;
    if(!next.save(prefs)){valid_=false;return false;}
    *this=next;return true;
  }
  template<class Preferences> bool revoke(Preferences& prefs,const std::string& id){
    uint8_t raw[8];if(!valid_||!decode(id,raw,8))return false;
    Registry next=*this;bool found=false;
    for(auto& client:next.clients_)if(client.used&&equal(client.id,raw,8)){client=Client{};found=true;}
    if(!found)return true;
    if(!next.save(prefs)){valid_=false;return false;}*this=next;return true;
  }
 private:
  static constexpr const char* KEY="lanAuthV1";
  Client clients_[CAPACITY];bool valid_=false;
  template<class Preferences> bool save(Preferences& prefs)const{
    uint8_t data[RECORD_BYTES]{},check[RECORD_BYTES];std::memcpy(data,"LAC1",4);data[4]=CAPACITY;size_t offset=8;
    for(const auto& client:clients_){
      std::memcpy(data+offset,client.id,8);offset+=8;std::memcpy(data+offset,client.verifier,32);offset+=32;
      data[offset++]=uint8_t(std::strlen(client.label));std::memcpy(data+offset,client.label,LABEL_BYTES);offset+=LABEL_BYTES;
    }
    const auto sum=crc(data,sizeof(data)-4);for(unsigned i=0;i<4;++i)data[sizeof(data)-4+i]=uint8_t(sum>>(8*i));
    return prefs.putBytes(KEY,data,sizeof(data))==sizeof(data)&&prefs.getBytesLength(KEY)==sizeof(data)&&
      prefs.getBytes(KEY,check,sizeof(check))==sizeof(check)&&equal(data,check,sizeof(data));
  }
};
}
