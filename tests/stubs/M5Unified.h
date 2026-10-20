#pragma once
#include "M5GFX.h"
struct TestDisplay {
 void setFont(const lgfx::IFont*){}
 void setTextSize(int){}
};
struct TestM5 {TestDisplay Display;};
inline TestM5 M5;
