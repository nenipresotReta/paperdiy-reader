#pragma once
#include <cstdint>
#include <openssl/sha.h>
inline int mbedtls_sha256_ret(const unsigned char* input,size_t bytes,unsigned char* output,int){SHA256(input,bytes,output);return 0;}
using mbedtls_sha256_context=SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context*){}
inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* context,int){return SHA256_Init(context)==1?0:-1;}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* context,const uint8_t* bytes,size_t count){return SHA256_Update(context,bytes,count)==1?0:-1;}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* context,uint8_t* digest){return SHA256_Final(digest,context)==1?0:-1;}
