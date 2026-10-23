#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int qrMatrix(const char* text,uint8_t* out,int version);
#ifdef __cplusplus
}
#endif
