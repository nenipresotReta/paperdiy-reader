#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
// Use the pinned library's actual typed RGB565/RGB888 conversion. The general
// UI fake intentionally omits grayscale and cannot detect this boundary bug.
#include <lgfx/v1/misc/colortype.hpp>

namespace lgfx { inline namespace v1 {
struct FontMetrics {
  int height=0,y_advance=0,baseline=0,y_offset=0,x_offset=0,width=0,x_advance=0;
};
struct TextStyle {float size_x=1,size_y=1;uint32_t fore_rgb888=0;};
class LGFXBase;
struct IFont {
  virtual ~IFont(){}
  virtual void getDefaultMetric(FontMetrics*) const=0;
  virtual bool updateFontMetric(FontMetrics*,uint16_t) const=0;
  virtual size_t drawChar(LGFXBase*,int32_t,int32_t,uint16_t,const TextStyle*,FontMetrics*,int32_t&) const=0;
};
class LGFXBase {
public:
  static constexpr int W=96,H=96;
  std::array<uint8_t,W*H> pixels;
  std::vector<uint32_t> rawColors;
  bool fast=false;
  int rotation=0;
  color_conv_t convert{color_depth_t::grayscale_8bit};
  void clear(uint8_t background){pixels.fill(background);rawColors.clear();}
  // Same Bayer table/thresholds as pinned platforms/esp32/Panel_EPD.inl.
  // Check native 4-bit gray, before SCREEN's nonzero-to-white conversion.
  static uint8_t quantize(uint32_t raw,int x,int y,bool fast){
    static const uint8_t bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    const int rank=bayer[(y&3)*4+(x&3)];
    return fast?(int(raw)+rank*16<248?0:15):std::min(15,std::max(0,(int(raw)+rank-8)>>4));
  }
  template<class T> void fillRect(int x,int y,int w,int h,T color){
    const uint32_t raw=convert.convert(color);rawColors.push_back(raw);
    for(int row=y;row<y+h;++row)for(int col=x;col<x+w;++col){
      assert(col>=0&&col<W&&row>=0&&row<H);
      int px=col,py=row;
      if(rotation&1)std::swap(px,py);
      if(rotation==1||rotation==2)px=W-1-px;
      if(rotation==2||rotation==3)py=H-1-py;
      pixels[row*W+col]=quantize(raw,px,py,fast);
    }
  }
  template<class T> void drawFastHLine(int x,int y,int w,T color){fillRect(x,y,w,1,color);}
};
}}
