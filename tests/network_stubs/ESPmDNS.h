#pragma once
#include <cstdint>
#include <string>
// Host stand-in for Arduino-ESP32 ESPmDNS: records lifecycle and advertisements.
struct MdnsStub {
  bool running=false,failBegin=false;unsigned begins=0,ends=0;
  std::string host,service,proto,txtKey,txtValue;uint16_t port=0;
  bool begin(const char* name){++begins;if(failBegin)return false;running=true;host=name;return true;}
  void end(){++ends;running=false;service.clear();proto.clear();txtKey.clear();txtValue.clear();port=0;}
  bool addService(const char* s,const char* p,uint16_t value){if(!running)return false;service=s;proto=p;port=value;return true;}
  void addServiceTxt(const char* s,const char* p,const char* key,const char* value){if(running&&service==s&&proto==p){txtKey=key;txtValue=value;}}
};
inline MdnsStub MDNS;
