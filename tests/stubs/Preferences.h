#pragma once
#include <cstdint>
#include <map>
#include <string>
struct Preferences {
 std::map<std::string,uint32_t> values;
 std::string last,failReadKey;
 bool failBegin=false,failWrite=false,failResume=false;
 unsigned writes=0;
 bool begin(const char*,bool){return !failBegin;}
 bool isKey(const char* key){return values.find(key)!=values.end();}
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
