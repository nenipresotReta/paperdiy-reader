#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
struct Preferences {
 std::map<std::string,String> strings;
 std::map<std::string,std::vector<uint8_t>> blobs;
 bool failWrite=false,shortRead=false;
 unsigned writes=0;
 String getString(const char* key,const char* fallback="")const{
   auto it=strings.find(key);return it==strings.end()?String(fallback):it->second;
 }
 size_t putString(const char* key,const String& value){
   ++writes;if(failWrite)return 0;strings[key]=value;return value.length();
 }
 size_t getBytesLength(const char* key)const{
   auto it=blobs.find(key);return it==blobs.end()?0:it->second.size();
 }
 size_t getBytes(const char* key,void* out,size_t len)const{
   auto it=blobs.find(key);if(it==blobs.end())return 0;
   len=std::min(len,it->second.size());if(shortRead&&len)--len;
   std::memcpy(out,it->second.data(),len);return len;
 }
 size_t putBytes(const char* key,const void* data,size_t len){
   ++writes;if(failWrite)return 0;
   const auto* bytes=static_cast<const uint8_t*>(data);blobs[key]={bytes,bytes+len};return len;
 }
 bool isKey(const char* key)const{return strings.count(key)||blobs.count(key);}
 bool remove(const char* key){return strings.erase(key)+blobs.erase(key)>0;}
};
