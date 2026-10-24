#pragma once
#include "console.h"
// Opt-in device diagnostic. Excluded from ordinary firmware builds.
// Caller: main application task, stable screen, no open import/cache/upload.
// Dispatch a numeric existing library index; do NOT call openSD or scanSD.
#include <Arduino.h>
#include <esp_timer.h>
#include <memory>
#include "bounded_sd_probe.h"
#include "sd_reader.h"
#include "network.h"
#include "power.h"
namespace sd_io_device_test {
inline uint64_t now(void*){return esp_timer_get_time();}
inline bool cancel(void*){return digitalRead(48)!=LOW||rawPowerKeys()!=0;}
inline void yield(void*){delay(1);}
inline bool run(size_t index,uint32_t verifiedMountHz=0,bool cachePrefix=false){
  if(!sdOK||networkActive()||index>=sdBooks.size()||sdBooks[index].length()>1024){
    Console.println("SD_IO_REJECT precondition=1");return false;
  }
  // stdio uses the same VFS backend as File::read without VFSImpl::open's
  // error log that can print a private filename. No filename/hash is emitted.
  char path[1030];
  int length=0;
  if(cachePrefix){
    uint8_t digest[32];
    if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(sdBooks[index].c_str()),sdBooks[index].length(),digest,0)){
      Console.println("SD_IO_REJECT path_hash=1");return false;
    }
    char hex[65];for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
    length=snprintf(path,sizeof(path),"/sd/.paperdiy-cache/%s.bin",hex);
  }else length=snprintf(path,sizeof(path),"/sd%s",sdBooks[index].c_str());
  if(length<0||size_t(length)>=sizeof(path)){Console.println("SD_IO_REJECT path_length=1");return false;}
  std::unique_ptr<uint8_t,decltype(&free)> buffer(static_cast<uint8_t*>(ps_malloc(bounded_sd_probe::CHUNK)),free);
  if(!buffer){Console.println("SD_IO_REJECT allocation=1");return false;}
  powerActivity();
  bounded_sd_probe::Hooks hooks;hooks.now=now;hooks.cancel=cancel;hooks.yield=yield;
  Console.printf("SD_IO_TARGET cache=%d\n",cachePrefix);
  const auto result=bounded_sd_probe::run(path,buffer.get(),bounded_sd_probe::CHUNK,hooks);
  Console.printf("SD_IO_SOURCE stat_ok=%d kind=%u regular=%d directory=%d size=%lld errno=%d stat_errno=%d open_errno=%d\n",
    result.statOk,unsigned(result.sourceKind),result.sourceKind==bounded_sd_probe::SourceKind::Regular,
    result.sourceKind==bounded_sd_probe::SourceKind::Directory,(long long)result.sourceSize,
    result.errorNumber,result.statError,result.openError);
  for(unsigned i=0;i<2;++i){const auto& p=result.pass[i];
    Console.printf("SD_IO_PASS pass=%u bytes=%u calls=%u read_us=%llu hash_us=%llu wall_us=%llu chunk=%u cpu_mhz=%u mount_hz=%u\n",
      i,unsigned(p.bytes),p.reads,(unsigned long long)p.readUs,(unsigned long long)p.hashUs,
      (unsigned long long)p.wallUs,unsigned(bounded_sd_probe::CHUNK),getCpuFrequencyMhz(),verifiedMountHz);
  }
  Console.printf("SD_IO_DONE status=%u prefix_hash_equal=%d total_us=%llu max_total_bytes=%u\n",
    unsigned(result.status),result.consistent,(unsigned long long)result.wallUs,unsigned(2*bounded_sd_probe::MAX_BYTES));
  return result.status==bounded_sd_probe::Status::Ok&&result.consistent;
}
}
