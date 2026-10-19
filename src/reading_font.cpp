#include <M5Unified.h>
#include "reading_font.h"
#include "reader_font.h"
#include "native_font_data.h"
namespace {
constexpr size_t COUNT=sizeof(GLYPHS_28)/sizeof(GLYPHS_28[0]);
const NativeGlyph* glyph(uint32_t cp,bool small=false){
  const auto* data=small?GLYPHS_24:GLYPHS_28;
  size_t lo=0,hi=COUNT;
  while(lo<hi){size_t mid=(lo+hi)/2;if(data[mid].code<cp)lo=mid+1;else hi=mid;}
  return lo<COUNT&&data[lo].code==cp?&data[lo]:nullptr;
}
int blankAdvance(uint32_t cp,int size){
  if(cp<32||cp==0xfeff||cp==0x200b||cp==0x200c||cp==0x200d||cp==0x2060||cp==0xad||
     (cp>=0x202a&&cp<=0x202e)||(cp>=0x2066&&cp<=0x2069)||(cp>=0xfe00&&cp<=0xfe0f))return 0;
  if(cp==0xa0||cp==0x2007)return size/2;
  if(cp==0x202f||cp==0x2009)return size/4;
  if(cp==0x2003)return size;
  return -1;
}
void paint(lgfx::LGFXBase* display,const NativeGlyph& g,int x,int y,uint32_t color,int size){
  for(int row=0;row<size;++row){
    int col=0;
    while(col<g.width){
      while(col<g.width&&!(g.rows[row]&(1u<<(31-col))))++col;
      int first=col;
      while(col<g.width&&(g.rows[row]&(1u<<(31-col))))++col;
      if(col>first)display->drawFastHLine(x+g.left+first,y+row,col-first,color);
    }
  }
}
class NativeFont:public lgfx::IFont {
  int size;
public:
  explicit NativeFont(int px):size(px){}
  void getDefaultMetric(lgfx::FontMetrics* m)const override{
    m->height=size;m->y_advance=size;m->baseline=size-4;m->y_offset=4-size;m->x_offset=0;m->width=m->x_advance=size;
  }
  bool updateFontMetric(lgfx::FontMetrics* m,uint16_t cp)const override{
    int blank=blankAdvance(cp,size);
    if(blank>=0){m->width=m->x_advance=blank;m->x_offset=0;return true;}
    if(auto g=glyph(cp,size==24)){m->width=g->width;m->x_advance=g->advance;m->x_offset=g->left;return true;}
    m->width=m->x_advance=size;m->x_offset=0;return false;
  }
  size_t drawChar(lgfx::LGFXBase* display,int32_t x,int32_t y,uint16_t cp,const lgfx::TextStyle* style,lgfx::FontMetrics* m,int32_t& filled)const override{
    y+=m->y_offset;
    int blank=blankAdvance(cp,size);if(blank>=0){filled=x+blank;return blank;}
    if(auto g=glyph(cp,size==24)){paint(display,*g,x,y,style->fore_rgb888,size);filled=x+g->advance;return g->advance;}
    return drawCharDummy(display,x,y,size,size,style,filled);
  }
};
NativeFont body(28),small(24);
int advance(uint32_t cp,int size){int blank=blankAdvance(cp,size);if(blank>=0)return blank;if(auto g=glyph(cp,size==24))return g->advance;return size;}
}
const lgfx::IFont* bodyFont(){return &body;}
int bodyGlyphAdvance(uint32_t cp){return advance(cp,28);}
bool bodyFontHasGlyph(uint32_t cp){return blankAdvance(cp,28)>=0||glyph(cp)!=nullptr;}
void drawBodyGlyph(const char* text,size_t bytes,int x,int y,uint32_t foreground){
  uint32_t cp=readerCodepoint(text,bytes);
  if(auto g=glyph(cp))paint(&M5.Display,*g,x,y,foreground,28);
  else if(blankAdvance(cp,28)<0){M5.Display.setCursor(x,y);M5.Display.print(text);}
}
const lgfx::IFont* readerFont(){return &small;}
int readerGlyphWidth(uint32_t cp){return advance(cp,24);}
bool readerFontHasGlyph(uint32_t cp){return blankAdvance(cp,24)>=0||glyph(cp,true)!=nullptr;}
uint32_t readerCodepoint(const char* text,size_t bytes){
  uint32_t cp=(uint8_t)text[0];if(bytes>1){cp&=(1u<<(7-bytes))-1;for(size_t i=1;i<bytes;++i)cp=(cp<<6)|((uint8_t)text[i]&63);}return cp;
}
void drawReaderGlyph(const char* text,size_t bytes,int x,int y,uint32_t foreground){
  uint32_t cp=readerCodepoint(text,bytes);
  if(auto g=glyph(cp,true))paint(&M5.Display,*g,x,y,foreground,24);
  else if(blankAdvance(cp,24)<0){M5.Display.setCursor(x,y);M5.Display.print(text);}
}
