#pragma once
#include "M5GFX.h"
#include "Arduino.h"
#ifndef TFT_BLACK
#define TFT_BLACK 0
#endif
struct TestDisplay {
 void (*observe)(int,int,int,int,uint32_t)=nullptr;
 void setFont(const lgfx::IFont*){}
 void setTextSize(int){}
 void fillRect(int x,int y,int w,int h,uint32_t color){if(observe)observe(x,y,w,h,color);}
 void drawFastHLine(int x,int y,int w,uint32_t color){if(observe)observe(x,y,w,1,color);}
};
struct TestM5 {TestDisplay Display;};
inline TestM5 M5;
