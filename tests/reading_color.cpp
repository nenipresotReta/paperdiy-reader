#include "M5Unified.h"
#include "reader_font.h"
#include "reading_font.h"
#include "native_font_data.h"
#include <cstdio>
#include <set>
#include <utility>

ColorTestM5 M5;
using Pixels=std::set<std::pair<int,int>>;
using Draw=void(*)(const char*,size_t,int,int,uint16_t);

static void glyph(Draw draw,const reader_font::Font& font,uint32_t cp,const char* utf8,size_t bytes){
  Pixels expected;
  reader_font::drawGlyphRuns(font,cp,24,24,[&](int x,int y,int n){
    for(int i=0;i<n;++i)expected.emplace(x+i,y);
  });
  assert(!expected.empty());
  for(int rotation=0;rotation<4;++rotation)for(bool fast:{false,true})for(bool white:{false,true}){
    auto& display=M5.Display;display.rotation=rotation;display.fast=fast;
    const uint8_t foreground=white?15:0,background=white?0:15;
    display.clear(background);
    draw(utf8,bytes,24,24,white?TFT_WHITE:TFT_BLACK);
    assert(!display.rawColors.empty());
    for(uint32_t raw:display.rawColors)assert(raw==unsigned(white?255:0));
    for(int y=0;y<display.H;++y)for(int x=0;x<display.W;++x)
      assert(display.pixels[y*display.W+x]==(expected.count({x,y})?foreground:background));
  }
}

static void styleColor(const lgfx::IFont* font){
  // This separate M5GFX callback already supplies RGB888. It must keep that
  // representation rather than being truncated with the explicit glyph API.
  lgfx::TextStyle style;lgfx::FontMetrics metrics;
  font->getDefaultMetric(&metrics);
  for(bool fast:{false,true})for(uint32_t rgb:{0u,0xFFFFFFu}){
    M5.Display.fast=fast;M5.Display.clear(rgb?0:15);style.fore_rgb888=rgb;
    int32_t filled=0;
    assert(font->drawChar(&M5.Display,24,24+metrics.baseline,'N',&style,&metrics,filled)>0);
    assert(!M5.Display.rawColors.empty());
    for(uint32_t raw:M5.Display.rawColors)assert(raw==(rgb?255u:0u));
  }
}

int main(){
  auto& display=M5.Display;
  assert(display.convert.convert(TFT_WHITE)==255);
  // Prove this test models the formerly hidden bug rather than mapping every
  // nonzero input to white, as the general monochrome UI fake does.
  const uint32_t wrongWhite=TFT_WHITE;
  const uint32_t wrongGray=display.convert.convert(wrongWhite);
  assert(wrongGray==179);
  unsigned blackInFast=0;
  for(int y=0;y<4;++y)for(int x=0;x<4;++x){
    blackInFast+=display.quantize(wrongGray,x,y,true)==0;
    assert(display.quantize(wrongGray,x,y,false)<15);
  }
  assert(blackInFast==5);
  glyph(drawReaderGlyph,reader_font::FONT_24,'N',"N",1);
  glyph(drawReaderGlyph,reader_font::FONT_24,0x4E2D,"\xE4\xB8\xAD",3);
  glyph(drawInterfaceGlyph,reader_font::FONT_28,'N',"N",1);
  for(uint8_t preset:{0,1,2}){
    reader_settings::current().preset=preset;
    glyph(drawBodyGlyph,bodyNativeFont(),'N',"N",1);
    styleColor(bodyFont());
  }
  styleColor(readerFont());
  std::puts("actual glyph renderer: RGB565 black/white and RGB888 font styles preserve full ink in fast/quality modes");
}
