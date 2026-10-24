#include "lan_auth.h"
#include <openssl/sha.h>
#include <cassert>
#include <iostream>
#include <map>
#include <vector>
namespace {
uint8_t counter=0;
bool randomBytes(uint8_t* out,size_t size){for(size_t i=0;i<size;++i)out[i]=++counter;return true;}
bool hash(const uint8_t* bytes,size_t size,uint8_t out[32]){return SHA256(bytes,size,out)!=nullptr;}
struct Store {
 std::map<std::string,std::vector<uint8_t>> blobs;
 bool stateError=false,shortRead=false,failWrite=false,writeThenFail=false;
 lan_nvs::KeyState keyState(const char* key){return stateError?lan_nvs::KeyState::Error:blobs.count(key)?lan_nvs::KeyState::Present:lan_nvs::KeyState::Absent;}
 size_t getBytesLength(const char* key){auto i=blobs.find(key);return i==blobs.end()?0:i->second.size();}
 size_t getBytes(const char* key,void* out,size_t size){auto i=blobs.find(key);if(shortRead||i==blobs.end()||i->second.size()>size)return 0;std::memcpy(out,i->second.data(),i->second.size());return i->second.size();}
 size_t putBytes(const char* key,const void* bytes,size_t size){if(failWrite)return 0;const auto* p=static_cast<const uint8_t*>(bytes);blobs[key]={p,p+size};return writeThenFail?0:size;}
};
}
int main(){
  Store prefs;lan_auth::Registry registry;
  prefs.stateError=true;assert(!registry.load(prefs)&&!registry.valid()&&prefs.blobs.empty());prefs.stateError=false;
  assert(registry.load(prefs)&&registry.count()==0);
  const auto original=prefs.blobs;
  std::string id,token,accepted;
  for(const auto& bad:std::vector<std::string>{"","bad\nlabel",std::string("bad\xff",4),std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),u8"C1\u0085",std::string(49,'x')})assert(!registry.add(prefs,bad,randomBytes,hash,id,token)&&prefs.blobs==original);
  assert(registry.add(prefs,"Synthetic client",randomBytes,hash,id,token));
  assert(id.size()==16&&token.size()==64&&registry.count()==1);
  std::string inventoryId,inventoryLabel;
  assert(registry.clientInfo(0,inventoryId,inventoryLabel)&&inventoryId==id&&inventoryLabel=="Synthetic client");
  assert(!registry.clientInfo(lan_auth::CAPACITY,inventoryId,inventoryLabel)&&inventoryId.empty()&&inventoryLabel.empty());
  const auto bearer="Bearer "+id+":"+token;
  assert(registry.authenticate(bearer,hash,accepted)&&accepted==id);
  assert(!registry.authenticate(bearer+" ",hash,accepted)&&accepted.empty());
  assert(!registry.authenticate("Bearer "+id+":"+std::string(64,'0'),hash,accepted));
  for(const auto& item:prefs.blobs){const auto& blob=item.second;const std::string raw(blob.begin(),blob.end());assert(raw.find(token)==std::string::npos);uint8_t secret[32];assert(lan_auth::decode(token,secret,32));assert(raw.find(std::string(reinterpret_cast<char*>(secret),32))==std::string::npos);}
  lan_auth::Registry rebooted;assert(rebooted.load(prefs)&&rebooted.authenticate(bearer,hash,accepted));
  // Any unsuccessful mutation needs verified reload, including a write whose
  // durable outcome is unknown. A revoked token must never remain live in RAM.
  prefs.failWrite=true;assert(!rebooted.revoke(prefs,id)&&!rebooted.valid()&&!rebooted.authenticate(bearer,hash,accepted));prefs.failWrite=false;
  assert(rebooted.load(prefs)&&rebooted.authenticate(bearer,hash,accepted));
  prefs.shortRead=true;assert(!rebooted.revoke(prefs,id)&&!rebooted.valid()&&!rebooted.authenticate(bearer,hash,accepted));prefs.shortRead=false;
  assert(rebooted.load(prefs)&&rebooted.count()==0); // durable revoke completed
  assert(rebooted.add(prefs,"Again",randomBytes,hash,id,token));
  prefs.writeThenFail=true;assert(!rebooted.revoke(prefs,id)&&!rebooted.valid());prefs.writeThenFail=false;
  assert(rebooted.load(prefs)&&rebooted.count()==0);
  std::string otherId,otherToken;
  prefs.failWrite=true;assert(!rebooted.add(prefs,"Write failure",randomBytes,hash,otherId,otherToken));assert(otherId.empty()&&otherToken.empty()&&!rebooted.valid());prefs.failWrite=false;assert(rebooted.load(prefs));
  for(unsigned i=0;i<lan_auth::CAPACITY;++i)assert(rebooted.add(prefs,std::string(16, 'a')+u8"日本日本日本日本"+"01234567",randomBytes,hash,otherId,otherToken)); // 48 UTF-8 bytes
  assert(!rebooted.add(prefs,"Overflow",randomBytes,hash,otherId,otherToken));
  auto good=prefs.blobs;for(auto& entry:prefs.blobs)entry.second[9]^=1;assert(!registry.load(prefs)&&!registry.valid()&&!registry.authenticate(bearer,hash,accepted));prefs.blobs=good;
  prefs.shortRead=true;assert(!registry.load(prefs));prefs.shortRead=false;prefs.stateError=true;assert(!registry.load(prefs)&&!registry.valid());
  std::cout<<"LAN tri-state persistence, uncertain-write fail-closed auth, UTF-8/C1 labels, capacity and corruption passed\n";
}
