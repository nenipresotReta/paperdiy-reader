#pragma once
#ifndef PAPERDIY_DEVICE_TESTS
#error SD_HASH is only available in explicit device-test builds
#endif
#include "console.h"
#include "sd_audit_file.h"
#include "sd_reader.h"
#include "network.h"
#include "lan_service.h"
#include "power.h"
#include "storage_validation.h"
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <memory>
namespace sd_audit_device {
struct Context {uint64_t lease;uint32_t started;};
inline bool allowed(void* p){
  const auto& c=*static_cast<Context*>(p);
  return sdOK&&!sdOwnedByUsb()&&!networkActive()&&!openSDPending()&&!sdBackgroundPending()&&
    !lan_service::storageBusy()&&digitalRead(48)==LOW&&!rawPowerKeys()&&
    c.lease&&c.lease==storage_validation::lease()&&uint32_t(millis()-c.started)<300000;
}
inline void cooperate(void*){delay(1);}
struct Hash {
  mbedtls_sha256_context context;
  Hash(){mbedtls_sha256_init(&context);}~Hash(){mbedtls_sha256_free(&context);}
  bool begin(){return mbedtls_sha256_starts_ret(&context,0)==0;}
  bool update(const uint8_t* p,size_t n){return mbedtls_sha256_update_ret(&context,p,n)==0;}
  bool finish(uint8_t* p){return mbedtls_sha256_finish_ret(&context,p)==0;}
};
inline void hexadecimal(const uint8_t* p,char (&out)[65]){for(unsigned i=0;i<32;++i)snprintf(out+i*2,3,"%02x",p[i]);}
inline void reject(const char* code,const char* identity="none"){
  Console.printf("SD_HASH_ERROR path_sha256=%s code=%s\n",identity,code);
  Console.printf("SD_HASH_DONE path_sha256=%s status=error\n",identity);
}
inline bool command(const String& line){
  if(line!="SD_HASH"&&!line.startsWith("SD_HASH "))return false;
  char path[sd_audit::MAX_PATH+1];
  const auto decoded=sd_audit::decode(line.length()>8?line.c_str()+8:nullptr,line.length()>8?line.length()-8:0,path);
  if(decoded!=sd_audit::Status::Ok){reject(sd_audit::name(decoded));return true;}
  uint8_t digest[32];char identity[65];
  if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(path),strlen(path),digest,0)){reject("hash");return true;}
  hexadecimal(digest,identity);
  Context context{storage_validation::lease(),millis()};
  if(!allowed(&context)){reject("guard",identity);return true;}
  std::unique_ptr<uint8_t,decltype(&free)> buffer(static_cast<uint8_t*>(ps_malloc(sd_audit::CHUNK)),free);
  if(!buffer){reject("allocation",identity);return true;}
  char nativePath[sd_audit::MAX_PATH+4];snprintf(nativePath,sizeof(nativePath),"/sd%s",path);
  // stdio avoids VFSImpl::open diagnostic logs containing private filenames.
  sd_audit::File io(nativePath,allowed,cooperate,&context);Hash hash;
  Console.printf("SD_HASH_BEGIN path_sha256=%s\n",identity);
  const auto result=sd_audit::run(io,hash,buffer.get(),sd_audit::CHUNK,sd_audit::HandleStampPolicy::TypeAndLength);
  if(result.status!=sd_audit::Status::Ok){reject(sd_audit::name(result.status),identity);return true;}
  char text[65];hexadecimal(result.digest,text);
  Console.printf("SD_HASH_RESULT path_sha256=%s bytes=%llu sha256=%s chunks=%u elapsed_ms=%u\n",
    identity,(unsigned long long)result.bytes,text,result.reads,unsigned(millis()-context.started));
  Console.printf("SD_HASH_DONE path_sha256=%s status=ok\n",identity);return true;
}
} // namespace sd_audit_device
