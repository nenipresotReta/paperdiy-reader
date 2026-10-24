#pragma once
#include <cstdint>
#include <cstddef>
using nvs_handle_t=uint32_t;using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NVS_TYPE_MISMATCH=2,ESP_FAIL=3,NVS_READWRITE=1;
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);
