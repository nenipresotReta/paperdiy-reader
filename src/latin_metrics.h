// Generated from Noto Serif CJK SC (SIL OFL 1.1). Do not edit.
// Source SHA-256: 2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca
#pragma once
#include <cstdint>
namespace latin_metrics {
constexpr int COUNT = 103;
extern const uint16_t ADVANCE_24[COUNT];
extern const int8_t KERN_24[COUNT * COUNT];
extern const uint16_t ADVANCE_28[COUNT];
extern const int8_t KERN_28[COUNT * COUNT];
inline int index(uint32_t cp) {
  if(cp>=32&&cp<=126)return int(cp)-32;
  switch(cp){
    case 0x2010:return 95;
    case 0x2013:return 96;
    case 0x2014:return 97;
    case 0x2018:return 98;
    case 0x2019:return 99;
    case 0x201c:return 100;
    case 0x201d:return 101;
    case 0x2026:return 102;
    default:return -1;
  }
}
inline int advance64(unsigned pixels,uint32_t cp,int fallback) {
  const int at=index(cp);
  return at<0?fallback*64:(pixels==24?ADVANCE_24[at]:ADVANCE_28[at]);
}
inline int kern64(unsigned pixels,uint32_t before,uint32_t after) {
  const int a=index(before),b=index(after);if(a<0||b<0)return 0;
  return int(pixels==24?KERN_24[a*COUNT+b]:KERN_28[a*COUNT+b])*4;
}
} // namespace latin_metrics
