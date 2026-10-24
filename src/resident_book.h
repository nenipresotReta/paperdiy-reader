#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace resident_book {
// A RAM view is valid only for one successfully opened source under the same
// exclusive storage generation and complete layout identity. A failed open of
// any source invalidates the snapshot before scratch text can be overwritten.
struct Snapshot {
  uint64_t generation=0;
  uint32_t layout=0;
  size_t bytes=0;
  char source[1025]={},bookmark[16]={};
  void clear(){generation=0;bytes=0;source[0]=bookmark[0]=0;}
  void remember(const char* path,const char* key,uint64_t epoch,uint32_t shape,size_t length){
    clear();if(!epoch||!path||!key||!length||std::strlen(path)>=sizeof(source)||std::strlen(key)>=sizeof(bookmark))return;
    std::strcpy(source,path);std::strcpy(bookmark,key);generation=epoch;layout=shape;bytes=length;
  }
  bool matches(const char* path,const char* key,uint64_t epoch,uint32_t shape,size_t length)const{
    return generation&&epoch==generation&&layout==shape&&length==bytes&&path&&key&&
      !std::strcmp(source,path)&&!std::strcmp(bookmark,key);
  }
};
}
