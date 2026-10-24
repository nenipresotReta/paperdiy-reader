#include "delivery_store_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <mbedtls/sha256.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include "storage_validation.h"
#include "markdown_document.h"

namespace delivery_store {
namespace {
struct Context {
  bool enabled=false;
  uint64_t lease=0;
  SDHooks hooks;
  mbedtls_sha256_context hash;
  Context(){mbedtls_sha256_init(&hash);}
  ~Context(){mbedtls_sha256_free(&hash);}
} context;
bool ready(void*){return context.enabled&&context.lease&&context.lease==storage_validation::lease()&&!storage_validation::cardEventPending();}
void mutation(){storage_validation::sourceChanged();context.lease=storage_validation::lease();}
Entry statFile(void*,const char* path,uint32_t* size){
  if(!ready(nullptr))return Entry::Error;
  struct stat info;const std::string full=std::string("/sd")+path;
  if(::stat(full.c_str(),&info)){return errno==ENOENT?Entry::Missing:Entry::Error;}
  if(S_ISDIR(info.st_mode))return Entry::Directory;
  if(!S_ISREG(info.st_mode)||info.st_size<0||uint64_t(info.st_size)>UINT32_MAX)return Entry::Error;
  *size=uint32_t(info.st_size);return Entry::File;
}
bool readFile(void*,const char* path,uint32_t at,uint8_t* bytes,size_t count){
  if(!ready(nullptr))return false;
  File file=SD.open(path,FILE_READ);
  const bool ok=file&&!file.isDirectory()&&file.seek(at)&&file.read(bytes,count)==count;file.close();
  return ok&&ready(nullptr);
}
bool writeFile(void*,const char* path,uint32_t at,const uint8_t* bytes,size_t count,bool truncate){
  if(!ready(nullptr))return false;mutation();
  File file=SD.open(path,truncate?"w":"r+");
  const bool ok=file&&!file.isDirectory()&&file.seek(at)&&(!count||file.write(bytes,count)==count);
  if(file)file.flush();file.close();return ok&&ready(nullptr);
}
bool owned(const char* path){constexpr char prefix[]="/.paperdiy/lan/";return path&&std::strncmp(path,prefix,sizeof(prefix)-1)==0;}
bool renameFile(void*,const char* from,const char* to,bool replaceOwned){
  if(!ready(nullptr)||!owned(from)||(replaceOwned&&!owned(to)))return false;
  uint32_t size;const auto entry=statFile(nullptr,to,&size);
  if(!replaceOwned&&entry!=Entry::Missing)return false;
  if(replaceOwned&&entry!=Entry::Missing&&entry!=Entry::File)return false;
  if(replaceOwned&&entry==Entry::File){mutation();if(!SD.remove(to)||!ready(nullptr))return false;}
  mutation();return SD.rename(from,to)&&ready(nullptr);
}
bool removeFile(void*,const char* path){
  if(!ready(nullptr)||!owned(path))return false;
  uint32_t size;const auto entry=statFile(nullptr,path,&size);
  if(entry==Entry::Missing)return true;if(entry!=Entry::File)return false;
  mutation();return SD.remove(path)&&ready(nullptr);
}
bool syncFile(void*,const char* path){
  if(!ready(nullptr))return false;
  const std::string full=std::string("/sd")+path;
  const int file=::open(full.c_str(),O_RDONLY);if(file<0)return false;
  const bool ok=::fsync(file)==0;const bool closed=::close(file)==0;
  return ok&&closed&&ready(nullptr);
}
bool admit(void*,const Metadata& m){return m.kind==Kind::Push||(context.hooks.admit&&context.hooks.admit(m));}
bool validatePush(void*,const uint8_t* bytes,size_t count){
  if(!ready(nullptr)||!bytes||!count||count>PUSH_LIMIT)return false;
  markdown::Document document;
  const bool valid=document.parse(reinterpret_cast<const char*>(bytes),count,[](void*){delay(1);});
  return valid&&ready(nullptr);
}
void published(void*,const Receipt& receipt,const char* path){if(context.hooks.published)context.hooks.published(receipt,path);}
bool hashBegin(void*){return mbedtls_sha256_starts_ret(&context.hash,0)==0;}
bool hashUpdate(void*,const uint8_t* bytes,size_t count){return mbedtls_sha256_update_ret(&context.hash,bytes,count)==0;}
bool hashFinish(void*,uint8_t out[32]){return mbedtls_sha256_finish_ret(&context.hash,out)==0;}
bool digest(void*,const uint8_t* bytes,size_t count,uint8_t out[32]){return mbedtls_sha256_ret(bytes,count,out,0)==0;}
IO makeIO(){
  IO io;io.stat=statFile;io.read=readFile;io.write=writeFile;io.rename=renameFile;io.remove=removeFile;io.sync=syncFile;
  io.ready=ready;io.admit=admit;io.validatePush=validatePush;io.published=published;return io;
}
Hash makeHash(){Hash hash;hash.begin=hashBegin;hash.update=hashUpdate;hash.finish=hashFinish;hash.digest=digest;return hash;}
bool directory(const char* path){
  uint32_t size;const auto entry=statFile(nullptr,path,&size);
  if(entry==Entry::Directory)return true;if(entry!=Entry::Missing)return false;
  mutation();return SD.mkdir(path)&&ready(nullptr);
}
}
Store& sdStore(){static Store store(makeIO(),makeHash());return store;}
bool beginSDStore(SDHooks hooks){
  context.hooks=hooks;context.lease=storage_validation::lease();context.enabled=context.lease!=0;
  if(!ready(nullptr)||!directory("/.paperdiy")||!directory("/.paperdiy/lan")||!directory("/Books")){context.enabled=false;return false;}
  sdStore().beginRecovery();return true;
}
void pollSDStore(){if(context.enabled)sdStore().poll();}
void stopSDStore(){if(context.enabled)sdStore().stop();context.enabled=false;context.lease=0;}
void invalidateSDStore(){context.enabled=false;context.lease=0;sdStore().stop();}
}
namespace delivery_store_sd {
delivery_store::Store& store(){return delivery_store::sdStore();}
void setHooks(delivery_store::SDHooks hooks){delivery_store::context.hooks=hooks;}
bool mount(){return delivery_store::beginSDStore(delivery_store::context.hooks);}
void unmount(){delivery_store::stopSDStore();}
bool mounted(){return delivery_store::ready(nullptr);}
void poll(){delivery_store::pollSDStore();}
void invalidate(){delivery_store::invalidateSDStore();}
}
#endif
