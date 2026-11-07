#pragma once
// A real directory stands in for the card, so src/delivery_store_sd.cpp runs
// its own File and POSIX calls. The adapter reaches the same directory through
// PAPERDIY_SD_VFS_ROOT. Only calls made through this stub are counted.
#include "Arduino.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
inline constexpr const char* FILE_READ="r";
inline std::string testCardRoot;
struct CardStub {
  unsigned calls=0,openFiles=0;
  bool failReceiptRecord=false;
  uint64_t totalBytes()const{return uint64_t(64)<<30;}
  uint64_t usedBytes()const{return 0;}
  class File open(const char* path,const char* mode=FILE_READ);
  bool mkdir(const char* path){++calls;return ::mkdir((testCardRoot+path).c_str(),0700)==0;}
  bool remove(const char* path){++calls;return ::unlink((testCardRoot+path).c_str())==0;}
  bool rename(const char* from,const char* to){++calls;return ::rename((testCardRoot+from).c_str(),(testCardRoot+to).c_str())==0;}
};
inline CardStub SD;
class File {
 public:
  File()=default;
  explicit File(int descriptor):descriptor_(descriptor){if(descriptor_>=0)++SD.openFiles;}
  explicit operator bool()const{return descriptor_>=0;}
  bool isDirectory()const{struct stat info;return descriptor_>=0&&!::fstat(descriptor_,&info)&&S_ISDIR(info.st_mode);}
  bool seek(uint32_t at){++SD.calls;return ::lseek(descriptor_,at,SEEK_SET)==off_t(at);}
  size_t read(uint8_t* out,size_t count){++SD.calls;const auto n=::read(descriptor_,out,count);return n<0?0:size_t(n);}
  size_t write(const uint8_t* bytes,size_t count){
    ++SD.calls;
    // A receipt is the only 1 KiB record of type 1; see Store::writeRecord.
    if(SD.failReceiptRecord&&count==1024&&bytes[4]==1)return 0;
    const auto n=::write(descriptor_,bytes,count);return n<0?0:size_t(n);
  }
  void flush(){}
  void close(){if(descriptor_>=0){::close(descriptor_);descriptor_=-1;--SD.openFiles;}}
 private:
  int descriptor_=-1;
};
inline File CardStub::open(const char* path,const char* mode){
  ++calls;
  const int flags=std::strcmp(mode,"w")==0?O_RDWR|O_CREAT|O_TRUNC:std::strcmp(mode,"r+")==0?O_RDWR:O_RDONLY;
  return File(::open((testCardRoot+path).c_str(),flags,0600));
}
