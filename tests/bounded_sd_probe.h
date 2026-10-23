#pragma once
// Read-only, opt-in diagnostic core. It emits nothing and never opens for write.
// Deadline is cooperative between stdio calls, not a cancellation of a blocked SDK read.
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/sha256.h>
namespace bounded_sd_probe {
constexpr size_t MAX_BYTES=256u*1024,CHUNK=8192;
constexpr uint64_t MAX_US=12u*1000*1000;
enum class Status:uint8_t {Ok,Invalid,Cancelled,Deadline,Open,NotRegular,Empty,Read,Changed,Hash,Close};
struct Hooks {
  uint64_t(*now)(void*)=nullptr;
  bool(*cancel)(void*)=nullptr;
  void(*yield)(void*)=nullptr;
  void* context=nullptr;
};
struct Pass {size_t bytes=0;uint32_t reads=0;uint64_t readUs=0,hashUs=0,wallUs=0;};
enum class SourceKind:uint8_t {Unknown,Regular,Directory,Other};
struct Result {
  Status status=Status::Invalid;Pass pass[2];bool consistent=false;uint64_t wallUs=0;
  bool statOk=false;SourceKind sourceKind=SourceKind::Unknown;int64_t sourceSize=-1;
  int errorNumber=0,statError=0,openError=0;
};
inline Result run(const char* path,uint8_t* buffer,size_t capacity,const Hooks& hooks){
  Result r;
  if(!path||!*path||!buffer||capacity<CHUNK||!hooks.now||!hooks.cancel||!hooks.yield)return r;
  const uint64_t started=hooks.now(hooks.context);
  uint8_t previous[32]={};off_t expectedSize=0;time_t expectedTime=0;
  r.status=Status::Ok;
  auto check=[&](){
    if(hooks.cancel(hooks.context)){r.status=Status::Cancelled;return false;}
    if(hooks.now(hooks.context)-started>=MAX_US){r.status=Status::Deadline;return false;}
    return true;
  };
  for(unsigned number=0;number<2&&r.status==Status::Ok;++number){
    if(!check())break;
    const uint64_t passStart=hooks.now(hooks.context);
    // FatFs maps file-open against a directory to ENOENT. Classify first,
    // because host fopen(directory) behavior is not portable to the device.
    struct stat source{};
    errno=0;
    const int statResult=::stat(path,&source);
    const int statError=statResult?errno:0;
    r.statOk=statResult==0;r.statError=statError;
    r.sourceKind=SourceKind::Unknown;r.sourceSize=-1;
    if(statResult){r.status=Status::Open;r.errorNumber=statError;r.pass[number].wallUs=hooks.now(hooks.context)-passStart;break;}
    r.sourceSize=source.st_size;
    r.sourceKind=S_ISREG(source.st_mode)?SourceKind::Regular:(S_ISDIR(source.st_mode)?SourceKind::Directory:SourceKind::Other);
    if(r.sourceKind!=SourceKind::Regular){r.status=Status::NotRegular;r.pass[number].wallUs=hooks.now(hooks.context)-passStart;break;}
    errno=0;
    FILE* file=std::fopen(path,"rb");
    const int openError=file?0:errno;
    r.openError=openError;
    if(!file){r.status=Status::Open;r.errorNumber=openError;r.pass[number].wallUs=hooks.now(hooks.context)-passStart;break;}
    struct stat before{},after{};
    errno=0;
    const int beforeResult=fstat(fileno(file),&before);
    const int beforeError=beforeResult?errno:0;
    if(beforeResult){r.status=Status::NotRegular;r.errorNumber=beforeError;}
    else if(!S_ISREG(before.st_mode))r.status=Status::NotRegular;
    else if(before.st_size<=0)r.status=Status::Empty;
    else if(number&&(before.st_size!=expectedSize||before.st_mtime!=expectedTime))r.status=Status::Changed;
    if(number==0){expectedSize=before.st_size;expectedTime=before.st_mtime;}
    const size_t limit=r.status==Status::Ok?size_t(std::min<uint64_t>(uint64_t(before.st_size),MAX_BYTES)):0;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    if(r.status==Status::Ok&&mbedtls_sha256_starts_ret(&sha,0)!=0)r.status=Status::Hash;
    Pass& p=r.pass[number];uint64_t lastYield=hooks.now(hooks.context);
    while(p.bytes<limit&&r.status==Status::Ok&&check()){
      const size_t count=std::min(CHUNK,limit-p.bytes);
      uint64_t at=hooks.now(hooks.context);
      errno=0;
      const size_t got=std::fread(buffer,1,count,file);
      const int readError=got!=count?errno:0;
      p.readUs+=hooks.now(hooks.context)-at;++p.reads;p.bytes+=got;
      if(got!=count){r.status=Status::Read;r.errorNumber=readError;break;}
      at=hooks.now(hooks.context);
      if(mbedtls_sha256_update_ret(&sha,buffer,got)!=0)r.status=Status::Hash;
      p.hashUs+=hooks.now(hooks.context)-at;
      if(hooks.now(hooks.context)-lastYield>=50000){hooks.yield(hooks.context);lastYield=hooks.now(hooks.context);}
    }
    uint8_t digest[32]={};
    if(r.status==Status::Ok&&mbedtls_sha256_finish_ret(&sha,digest)!=0)r.status=Status::Hash;
    mbedtls_sha256_free(&sha);
    if(r.status==Status::Ok){
      errno=0;
      const int afterResult=fstat(fileno(file),&after);
      const int afterError=afterResult?errno:0;
      if(afterResult){r.status=Status::Changed;r.errorNumber=afterError;}
      else if(before.st_size!=after.st_size||before.st_mtime!=after.st_mtime)r.status=Status::Changed;
    }
    errno=0;
    const int closeResult=std::fclose(file);
    const int closeError=closeResult?errno:0;
    if(closeResult&&r.status==Status::Ok){r.status=Status::Close;r.errorNumber=closeError;}
    p.wallUs=hooks.now(hooks.context)-passStart;
    if(r.status==Status::Ok){
      if(number==0)std::memcpy(previous,digest,sizeof(previous));
      else r.consistent=p.bytes==r.pass[0].bytes&&!std::memcmp(previous,digest,sizeof(previous));
      check();
    }
  }
  r.wallUs=hooks.now(hooks.context)-started;
  if(r.status!=Status::Ok)r.consistent=false;
  return r;
}
}
