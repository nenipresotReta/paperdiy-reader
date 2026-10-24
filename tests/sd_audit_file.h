#pragma once
#include "sd_audit_core.h"
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
namespace sd_audit {
struct File {
  const char* path;bool (*allowed)(void*);void (*cooperate)(void*);void* context;
  FILE* file=nullptr;
  File(const char* p,bool (*a)(void*),void (*y)(void*),void* c):path(p),allowed(a),cooperate(y),context(c){}
  ~File(){if(file)std::fclose(file);}
  bool guard(){return allowed&&allowed(context);}
  static bool stamp(const struct stat& s,Stamp& out){
    if(s.st_size<0)return false;out.bytes=uint64_t(s.st_size);out.modified=s.st_mtime;out.changed=s.st_ctime;
    out.inode=s.st_ino;out.regular=S_ISREG(s.st_mode);return true;
  }
  bool pathStat(Stamp& out){struct stat s={};return !::stat(path,&s)&&stamp(s,out);}
  bool fileStat(Stamp& out){struct stat s={};return file&&!::fstat(fileno(file),&s)&&stamp(s,out);}
  bool open(){file=std::fopen(path,"rb");return file;}
  size_t read(uint8_t* p,size_t n){return std::fread(p,1,n,file);}
  bool error(){return std::ferror(file)!=0;}
  bool close(){if(!file)return true;FILE* f=file;file=nullptr;return std::fclose(f)==0;}
  void yield(){if(cooperate)cooperate(context);}
};
} // namespace sd_audit
