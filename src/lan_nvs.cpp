#include "lan_nvs.h"
#if defined(ARDUINO)||defined(PAPERDIY_NVS_TEST)
#include <nvs.h>
namespace lan_nvs {
bool Store::open(){if(opened_)return true;nvs_handle_t next=0;if(nvs_open("paperlan",NVS_READWRITE,&next)!=ESP_OK)return false;handle_=next;opened_=true;return true;}
void Store::close(){if(opened_)nvs_close(handle_);handle_=0;opened_=false;}
KeyState Store::keyState(const char* key)const{
  if(!opened_||!key)return KeyState::Error;size_t length=0;const auto status=nvs_get_blob(handle_,key,nullptr,&length);
  return status==ESP_OK?KeyState::Present:status==ESP_ERR_NVS_NOT_FOUND?KeyState::Absent:KeyState::Error;
}
size_t Store::getBytesLength(const char* key)const{if(!opened_||!key)return 0;size_t length=0;return nvs_get_blob(handle_,key,nullptr,&length)==ESP_OK?length:0;}
size_t Store::getBytes(const char* key,void* out,size_t capacity)const{if(!opened_||!key||!out||!capacity)return 0;size_t length=capacity;return nvs_get_blob(handle_,key,out,&length)==ESP_OK?length:0;}
size_t Store::putBytes(const char* key,const void* bytes,size_t length){if(!opened_||!key||!bytes||!length)return 0;return nvs_set_blob(handle_,key,bytes,length)==ESP_OK&&nvs_commit(handle_)==ESP_OK?length:0;}
}
#else
namespace lan_nvs {
bool Store::open(){return false;}void Store::close(){handle_=0;opened_=false;}
KeyState Store::keyState(const char*)const{return KeyState::Error;}
size_t Store::getBytesLength(const char*)const{return 0;}
size_t Store::getBytes(const char*,void*,size_t)const{return 0;}
size_t Store::putBytes(const char*,const void*,size_t){return 0;}
}
#endif
