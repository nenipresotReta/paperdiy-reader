#include "lan_nvs.h"
#include <nvs.h>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
namespace {std::map<std::string,std::vector<uint8_t>> data;int openResult=ESP_OK,getResult=ESP_OK,setResult=ESP_OK,commitResult=ESP_OK;unsigned opens=0,closes=0;}
esp_err_t nvs_open(const char* name,int mode,nvs_handle_t* handle){assert(!std::strcmp(name,"paperlan")&&mode==NVS_READWRITE);++opens;if(openResult)return openResult;*handle=17;return ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==17);++closes;}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char* key,void* out,size_t* length){assert(handle==17);if(getResult)return getResult;auto i=data.find(key);if(i==data.end())return ESP_ERR_NVS_NOT_FOUND;if(out){if(*length<i->second.size())return ESP_FAIL;std::memcpy(out,i->second.data(),i->second.size());}*length=i->second.size();return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char* key,const void* bytes,size_t length){assert(handle==17);if(setResult)return setResult;const auto* p=static_cast<const uint8_t*>(bytes);data[key]={p,p+length};return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){assert(handle==17);return commitResult;}
int main(){using S=lan_nvs::KeyState;lan_nvs::Store store;assert(!store.ready()&&store.keyState("lanTlsV1")==S::Error);openResult=ESP_FAIL;assert(!store.open()&&!store.ready());openResult=ESP_OK;assert(store.open()&&store.ready());assert(store.open()&&opens==2);assert(store.keyState("lanTlsV1")==S::Absent);
 const uint8_t identity[]={1,2,3};assert(store.putBytes("lanTlsV1",identity,3)==3&&store.keyState("lanTlsV1")==S::Present&&store.getBytesLength("lanTlsV1")==3);uint8_t copy[3];assert(store.getBytes("lanTlsV1",copy,3)==3&&!std::memcmp(identity,copy,3));
 for(int error:{ESP_FAIL,ESP_ERR_NVS_TYPE_MISMATCH}){getResult=error;assert(store.keyState("lanTlsV1")==S::Error&&store.getBytesLength("lanTlsV1")==0&&store.getBytes("lanTlsV1",copy,3)==0);}getResult=ESP_OK;
 setResult=ESP_FAIL;assert(!store.putBytes("x",identity,3));setResult=ESP_OK;commitResult=ESP_FAIL;assert(!store.putBytes("x",identity,3));commitResult=ESP_OK;store.close();assert(closes==1&&!store.ready()&&store.keyState("x")==S::Error);store.close();assert(closes==1);
 std::puts("LAN-only namespace, exact missing/error distinction, open/read/set/commit faults passed");}
