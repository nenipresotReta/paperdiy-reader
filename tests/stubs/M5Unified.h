#pragma once
#include "M5GFX.h"
#include "Arduino.h"
#ifndef TFT_BLACK
#define TFT_BLACK 0
#endif
struct TestDisplay {
 void setFont(const lgfx::IFont*){}
 void setTextSize(int){}
 void fillRect(int,int,int,int,uint32_t){}
 void drawFastHLine(int,int,int,uint32_t){}
};
struct TestM5 {TestDisplay Display;};
inline TestM5 M5;
