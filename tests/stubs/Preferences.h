#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <cstring>
struct Preferences {
 std::map<std::string,uint32_t> values;
 std::map<std::string,std::vector<uint8_t>> blobs;
 std::string last,failReadKey;
 bool failBegin=false,failWrite=false,failResume=false;
 unsigned writes=0;
 bool begin(const char*,bool){return !failBegin;}
 bool isKey(const char* key){return values.find(key)!=values.end()||blobs.find(key)!=blobs.end();}
 size_t getBytesLength(const char* key){if(failReadKey==key)return 0;auto it=blobs.find(key);return it==blobs.end()?0:it->second.size();}
 size_t getBytes(const char* key,void* target,size_t bytes){
   if(failReadKey==key)return 0;auto it=blobs.find(key);if(it==blobs.end()||it->second.size()>bytes)return 0;
   std::memcpy(target,it->second.data(),it->second.size());return it->second.size();
 }
 size_t putBytes(const char* key,const void* data,size_t bytes){
   ++writes;if(failWrite)return 0;const auto* p=static_cast<const uint8_t*>(data);blobs[key]={p,p+bytes};return bytes;
 }
 uint32_t getUInt(const char* key,uint32_t fallback){
   if(failReadKey==key)return fallback;
   auto found=values.find(key);return found==values.end()?fallback:found->second;
 }
 size_t putUInt(const char* key,uint32_t value){
   ++writes;if(failWrite)return 0;values[key]=value;return sizeof(value);
 }
 std::string getString(const char*,const char*){return last;}
 size_t putString(const char*,const char* value){
   ++writes;if(failResume)return 0;last=value;return last.size();
 }
};
