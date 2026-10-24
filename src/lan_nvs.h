#pragma once
#include <cstddef>
#include <cstdint>
namespace lan_nvs {
enum class KeyState:uint8_t{Absent,Present,Error};
// A separate namespace owns LAN identity and verifiers. Absence is reported
// only for ESP_ERR_NVS_NOT_FOUND; read/type/open errors never authorize reset.
class Store {
 public:
  Store()=default;~Store(){close();}
  Store(const Store&)=delete;Store& operator=(const Store&)=delete;
  bool open();void close();bool ready()const{return opened_;}
  KeyState keyState(const char* key)const;
  size_t getBytesLength(const char* key)const;
  size_t getBytes(const char* key,void* out,size_t capacity)const;
  size_t putBytes(const char* key,const void* bytes,size_t length);
 private:
  uint32_t handle_=0;bool opened_=false;
};
}
