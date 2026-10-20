#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using String=std::string;
inline uint32_t testClock=0;
inline uint32_t millis(){return testClock;}
inline void delay(uint32_t elapsed){testClock+=elapsed;}
struct TestSerial {
 void println(const char*){}
 template<class... Args>void printf(const char*,Args...){}
};
inline TestSerial Serial;
