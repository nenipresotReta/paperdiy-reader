#pragma once
#include "font_metrics.h"
#include <algorithm>
namespace vertical_font {
const reader_font::Font& font(unsigned pixels);
bool sideways(uint32_t cp,unsigned pixels=28);
constexpr int SIDEWAYS_EXTRA=3;
inline bool has(uint32_t cp,unsigned pixels){return reader_font::findGlyph(font(pixels),cp)!=nullptr;}
// Pixel-native cells, with JAN locl/vrt2 substitutions already baked.
// Tiny ruby permits up to two extra vertical pixels for native descenders.
// Sideways body ink has a bounded three-pixel allowance reserved by the composer.
// Fallback glyphs share Noto's 880/1000 vertical origin and native advances.
// Emit rectangles so sideways strokes do not need an intermediate bitmap.
template<class Emit>
void draw(const reader_font::Font& fallback,uint32_t cp,unsigned pixels,int x,int y,bool rotated,Emit emit){
  const auto& selected=font(pixels);const auto* own=rotated?nullptr:reader_font::findGlyph(selected,cp);
  const auto& face=own?selected:fallback;const auto* glyph=own?own:reader_font::findGlyph(face,cp);
  if(reader_font::blankAdvance(cp,pixels)>=0||cp==' '||cp==0x3000)return;
  const bool extended=rotated&&pixels>=20;
  const int cellWidth=int(pixels)+(extended?SIDEWAYS_EXTRA:0);
  const int cellHeight=own&&pixels<20?glyph->advance:int(pixels)+(extended?SIDEWAYS_EXTRA:0);
  if(!glyph||glyph->width>cellWidth||glyph->height>cellHeight){
    const int side=int(pixels)-4;emit(x+2,y+2,side,1);emit(x+2,y+side+1,side,1);emit(x+2,y+3,1,side-2);emit(x+side+1,y+3,1,side-2);return;
  }
  int left=glyph->left,top=own?glyph->top:int((pixels*880+500)/1000)+glyph->top;
  if(!own&&!rotated)left+=(int(pixels)-glyph->advance)/2;
  // Combining accents beyond the one-em native profile retain all their ink.
  // Repositioning is preferable to clipping; no resampling is performed.
  left=std::max(0,std::min(left,cellWidth-glyph->width));top=std::max(0,std::min(top,cellHeight-glyph->height));
  for(int row=0;row<glyph->height;++row)for(int col=0;col<glyph->width;){
    while(col<glyph->width&&!reader_font::inkAt(face,*glyph,col,row))++col;const int start=col;
    while(col<glyph->width&&reader_font::inkAt(face,*glyph,col,row))++col;if(col==start)continue;
    if(rotated)emit(x+int(pixels)-1-(top+row),y+left+start,1,col-start);
    else emit(x+left+start,y+top+row,col-start,1);
  }
}
} // namespace vertical_font
