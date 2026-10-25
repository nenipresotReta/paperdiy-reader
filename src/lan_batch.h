#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "delivery_store.h"

// Pure LAN v2 batch logic: packed entry parsing, pairing SAS and status proof.
// No Arduino, filesystem or network access; host tests exercise it directly.
namespace lan_batch {
constexpr size_t MAX_ENTRIES=8,PROMPT_NAMES=5,PACKED_MAX=1024;
constexpr uint32_t PROMPT_MS=60000,REVEAL_MS=60000;
using Digest=bool(*)(const uint8_t*,size_t,uint8_t[32]);

inline bool decodeHex(const std::string& text,uint8_t* out,size_t bytes){
  if(!delivery_store::hex(text,bytes*2))return false;
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(size_t i=0;i<bytes;++i)out[i]=uint8_t(digit(text[2*i])*16+digit(text[2*i+1]));
  return true;
}
inline bool canonicalDecimal(const std::string& text,uint32_t& out){
  if(text.empty()||text.size()>10||(text.size()>1&&text[0]=='0'))return false;
  uint64_t value=0;
  for(char c:text){if(c<'0'||c>'9')return false;value=value*10+uint64_t(c-'0');}
  if(value>UINT32_MAX)return false;
  out=uint32_t(value);return true;
}

// One entry is eight length-prefixed fields in fixed order, with no other
// separators: "<len>:<bytes>" repeated. Lengths are canonical decimals and
// bounded per field, so values are never scanned for delimiters.
//   requestId fingerprint kind name title size sha256 baseRevision
// baseRevision is empty for files. clientId is filled in by the caller.
inline bool parseEntry(const std::string& packed,delivery_store::Metadata& out){
  static constexpr size_t limits[8]={32,64,4,96,96,10,64,10};
  if(packed.empty()||packed.size()>PACKED_MAX)return false;
  std::string fields[8];size_t at=0;
  for(size_t field=0;field<8;++field){
    size_t length=0,digits=0;
    while(at<packed.size()&&packed[at]>='0'&&packed[at]<='9'){
      if(++digits>3)return false;
      length=length*10+size_t(packed[at++]-'0');
    }
    if(!digits||(digits>1&&packed[at-digits]=='0')||at>=packed.size()||packed[at]!=':')return false;
    ++at;
    if(length>limits[field]||length>packed.size()-at)return false;
    fields[field].assign(packed,at,length);at+=length;
  }
  if(at!=packed.size())return false;
  delivery_store::Metadata m;
  m.requestId=fields[0];m.fingerprint=fields[1];m.name=fields[3];m.title=fields[4];m.sha256=fields[6];
  if(fields[2]=="push")m.kind=delivery_store::Kind::Push;
  else if(fields[2]=="file")m.kind=delivery_store::Kind::File;
  else return false;
  if(!canonicalDecimal(fields[5],m.size))return false;
  if(m.kind==delivery_store::Kind::Push){if(!canonicalDecimal(fields[7],m.baseRevision))return false;}
  else if(!fields[7].empty())return false;
  if(!delivery_store::hex(m.requestId,32)||!delivery_store::hex(m.fingerprint,64)||!delivery_store::hex(m.sha256,64))return false;
  if(!delivery_store::label(m.name,true)||!delivery_store::label(m.title))return false;
  out=m;return true;
}
inline std::string packEntry(const delivery_store::Metadata& m){
  const std::string values[8]={m.requestId,m.fingerprint,m.kind==delivery_store::Kind::Push?"push":"file",m.name,m.title,
    std::to_string(m.size),m.sha256,m.kind==delivery_store::Kind::Push?std::to_string(m.baseRevision):std::string()};
  std::string out;for(const auto& v:values){out+=std::to_string(v.size());out+=':';out+=v;}
  return out;
}

inline bool hmac(Digest digest,const uint8_t* key,size_t keyBytes,const std::string& message,uint8_t out[32]){
  if(!digest||keyBytes>64)return false;
  uint8_t block[64]={};std::memcpy(block,key,keyBytes);
  std::string inner(64,'\0'),outer(64,'\0');
  for(size_t i=0;i<64;++i){inner[i]=char(block[i]^0x36);outer[i]=char(block[i]^0x5c);}
  inner+=message;
  uint8_t innerDigest[32];
  if(!digest(reinterpret_cast<const uint8_t*>(inner.data()),inner.size(),innerDigest))return false;
  outer.append(reinterpret_cast<const char*>(innerDigest),32);
  const bool ok=digest(reinterpret_cast<const uint8_t*>(outer.data()),outer.size(),out);
  volatile uint8_t* wipe=block;for(size_t i=0;i<64;++i)wipe[i]=0;
  return ok;
}
inline bool equalTime(const std::string& a,const std::string& b){
  if(a.size()!=b.size())return false;
  volatile uint8_t difference=0;
  for(size_t i=0;i<a.size();++i)difference=uint8_t(difference|(uint8_t(a[i])^uint8_t(b[i])));
  return difference==0;
}

// SAS = uint32_be(SHA-256("paperdiy-pair-v2\0" | F | Ns | Nd | pairingId | len16(label) | label)[0:4]) mod 10^6.
// F, Ns and Nd are raw 32-byte values; pairingId is its 16 ASCII hex digits.
inline bool sas(Digest digest,const uint8_t fingerprint[32],const uint8_t senderNonce[32],const uint8_t deviceNonce[32],
                const std::string& pairingId,const std::string& label,uint32_t& out){
  if(!digest||!delivery_store::hex(pairingId,16)||label.size()>0xffff)return false;
  std::string message("paperdiy-pair-v2",16);message+='\0';
  message.append(reinterpret_cast<const char*>(fingerprint),32);
  message.append(reinterpret_cast<const char*>(senderNonce),32);
  message.append(reinterpret_cast<const char*>(deviceNonce),32);
  message+=pairingId;message+=char(label.size()>>8);message+=char(label.size()&0xff);message+=label;
  uint8_t value[32];
  if(!digest(reinterpret_cast<const uint8_t*>(message.data()),message.size(),value))return false;
  out=(uint32_t(value[0])<<24|uint32_t(value[1])<<16|uint32_t(value[2])<<8|value[3])%1000000u;
  return true;
}
// Status proof = hex(HMAC-SHA256(Ns, "paperdiy-batch-status-v2" | batchId)).
inline bool statusProof(Digest digest,const uint8_t senderNonce[32],const std::string& batchId,std::string& out){
  if(!delivery_store::hex(batchId,16))return false;
  uint8_t value[32];
  if(!hmac(digest,senderNonce,32,std::string("paperdiy-batch-status-v2")+batchId,value))return false;
  out=delivery_store::hexadecimal(value,32);return true;
}
inline bool commitment(Digest digest,const uint8_t senderNonce[32],std::string& out){
  uint8_t value[32];if(!digest||!digest(senderNonce,32,value))return false;
  out=delivery_store::hexadecimal(value,32);return true;
}
inline std::string sasText(uint32_t value){
  char text[16];std::snprintf(text,sizeof(text),"%03u %03u",unsigned(value/1000),unsigned(value%1000));return text;
}
}
