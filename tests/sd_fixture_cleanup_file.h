#pragma once
#include "sd_fixture_cleanup.h"
#include "sd_fixture_file.h"
namespace sd_fixture {
struct CleanupFile : File {
  const char* cacheBase;const char* cacheDirectory;
  CleanupFile(const char* source,const char* base,const char* directory,bool (*allowed)(void*),void (*cooperate)(void*),void* context,bool (*mutation)(void*)=nullptr)
    :File(source,allowed,cooperate,context,mutation),cacheBase(base),cacheDirectory(directory){}
  static int noFollowStat(const char* path,struct stat* out){
#ifdef ARDUINO
    // This adapter is mounted only on FAT, which has no symlink objects. ESP
    // VFS has stat but no lstat callback; host tests use real no-follow lstat.
    return ::stat(path,out);
#else
    return ::lstat(path,out);
#endif
  }
  bool target(size_t index,char (&out)[256]){
    if(index>CACHE_COUNT)return false;
    const int n=index==CACHE_COUNT?snprintf(out,sizeof(out),"%s",path):snprintf(out,sizeof(out),"%s%s",cacheBase,cacheSuffix(index));
    return n>0&&size_t(n)<sizeof(out);
  }
  Inspect inspect(size_t index){
    char selected[256];if(!target(index,selected))return Inspect::Invalid;
    if(index<CACHE_COUNT){
      struct stat directory={};
      if(noFollowStat(cacheDirectory,&directory))return errno==ENOENT?Inspect::Missing:Inspect::Error;
      if(!S_ISDIR(directory.st_mode))return Inspect::Invalid;
    }
    struct stat info={};if(noFollowStat(selected,&info))return errno==ENOENT?Inspect::Missing:Inspect::Error;
    return S_ISREG(info.st_mode)&&info.st_size>=0&&uint64_t(info.st_size)<=sd_audit::MAX_BYTES?Inspect::Regular:Inspect::Invalid;
  }
  bool removeOne(size_t index){char selected[256];return target(index,selected)&&!::unlink(selected);}
};
} // namespace sd_fixture
