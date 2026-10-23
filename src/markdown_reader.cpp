#include "markdown_reader.h"
#include "markdown_document.h"
#include "markdown_pagination.h"
#include "math_layout.h"
#include "math_font.h"
#include "reading_font.h"
#include "reader_font.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <utility>
#include <memory>
#include <new>

namespace {
markdown::Document document;
math_layout::Workspace mathWorkspace;
constexpr unsigned FORMULA_SLOTS=8;
std::unique_ptr<math_layout::Command[]> formulaStorage;
bool formulaFailed=false;
int bodyAscii[128],smallAscii[128];bool bodyAsciiExists[128];
math_layout::Command* slot(unsigned index){
  return formulaStorage.get()+index*math_layout::MAX_COMMANDS;
}
uint32_t lastYield=0;
void cooperate(void*){
  if(uint32_t(millis()-lastYield)>=8){delay(1);lastYield=millis();}
}
bool mathCooperate(void*){cooperate(nullptr);return true;}
math_layout::Options options(bool display){
  math_layout::Options value;value.displayStyle=display;value.maxWidth=486;
  value.maxHeight=840;value.keepGoing=mathCooperate;return value;
}
bool acceptMath(const char* source,size_t bytes,bool display,void*){
  return math_layout::layoutInto(source,bytes,options(display),mathWorkspace,
    slot(0),math_layout::MAX_COMMANDS).ok();
}
struct CachedFormula {
  uint32_t begin=UINT32_MAX,end=0;
  math_layout::Result result;
};
CachedFormula formulas[FORMULA_SLOTS];
unsigned nextFormula=0;
const math_layout::Result& formula(const markdown::Run& run){
  for(const auto& cached:formulas)
    if(cached.begin==run.formulaBegin&&cached.end==run.formulaEnd)return cached.result;
  const unsigned index=nextFormula++%FORMULA_SLOTS;
  auto& cached=formulas[index];
  cached.begin=run.formulaBegin;cached.end=run.formulaEnd;
  cached.result=math_layout::layoutInto(document.source()+run.formulaBegin,
    run.formulaEnd-run.formulaBegin,options(run.kind==markdown::Kind::MathBlock),
    mathWorkspace,slot(index),math_layout::MAX_COMMANDS);
  return cached.result;
}
uint32_t alternateGlyph(const markdown::Run& run,uint32_t cp){
  if((run.flags&markdown::Emphasis)&&!(run.flags&markdown::Code)&&
      ((cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z'))){
    uint32_t styled=math_font::styled(cp,math_font::Style::Italic);
    if(math_font::find(styled,28))return styled;
  }
  if(!(run.flags&markdown::Code)&&!(cp<128?bodyAsciiExists[cp]:bodyFontHasGlyph(cp))&&math_font::find(cp,28))return cp;
  return 0;
}
struct Renderer {
  markdown_page::Box glyph(const markdown::Run& run,const char* ch,size_t bytes){
    uint32_t cp=readerCodepoint(ch,bytes);markdown_page::Box box;
    const bool code=(run.flags&markdown::Code)!=0;
    box.ascent=code?26:BODY_FONT_BASELINE;box.descent=code?8:BODY_FONT_HEIGHT-BODY_FONT_BASELINE;
    const uint32_t emphasis=alternateGlyph(run,cp);
    if(emphasis){
      auto g=math_font::find(emphasis,28);box.width=g->advance+std::max<int>(0,g->italicCorrection);
      box.ascent=std::max<int>(box.ascent,-g->top);
      box.descent=std::max<int>(box.descent,g->top+g->height);
      box.ink=g->width&&g->height;box.inkLeft=g->left;box.inkRight=g->left+g->width;
    }else box.width=code?(cp<32?0:std::max(0,cp<128?smallAscii[cp]:readerGlyphWidth(cp))):(cp<128?bodyAscii[cp]:bodyGlyphAdvance(cp));
    if(!emphasis&&!code&&typography_experiment::punctuation(cp)!=typography_experiment::Punctuation::None){
      if(const auto* g=bodyGlyph(cp)){box.ink=g->width&&g->height;box.inkLeft=g->left;box.inkRight=g->left+g->width;}
    }
    if(box.ink&&(run.flags&(markdown::Strong|markdown::Heading)))++box.inkRight;
    return box;
  }
  markdown_page::Box math(const markdown::Run& run){
    const auto& value=formula(run);markdown_page::Box box;
    if(!value.ok())formulaFailed=true;
    box.width=value.width;box.ascent=value.ascent;box.descent=value.descent;return box;
  }
  void rule(int x,int y,int width,int height){M5.Display.fillRect(x,y,width,height,TFT_BLACK);}
  void drawMath(const markdown::Run& run,int x,int baseline){
    const auto& value=formula(run);
    if(!value.ok()){formulaFailed=true;return;}
    const int y=baseline-value.ascent;
    for(const auto& command:value.commands){
      if(command.kind==math_layout::CommandKind::Rule)
        rule(x+command.x,y+command.y,command.width,command.height);
      else math_font::drawGlyphRuns(command.codepoint,command.pixels,x+command.x,y+command.y,
        [](int left,int top,int length){M5.Display.drawFastHLine(left,top,length,TFT_BLACK);});
    }
  }
  void drawGlyph(const markdown::Run& run,const char* ch,size_t bytes,int x,int baseline){
    const uint32_t cp=readerCodepoint(ch,bytes);if(cp<32)return;
    const bool code=(run.flags&markdown::Code)!=0;
    const uint32_t emphasis=alternateGlyph(run,cp);
    auto paint=[&](int left){
      if(emphasis)math_font::drawGlyphRuns(emphasis,28,left,baseline,
        [](int xx,int yy,int width){M5.Display.drawFastHLine(xx,yy,width,TFT_BLACK);});
      else if(code)drawReaderGlyph(ch,bytes,left,baseline-26);
      else drawBodyGlyph(ch,bytes,left,baseline-BODY_FONT_BASELINE);
    };
    paint(x);
    if(run.flags&(markdown::Strong|markdown::Heading))paint(x+1);
    const int width=glyph(run,ch,bytes).width;
    if((run.flags&markdown::Link)||((run.flags&markdown::Emphasis)&&!emphasis&&cp>127))
      rule(x,baseline+2,width,1);
    if(run.flags&markdown::Strike)rule(x,baseline-10,width,1);
  }
};
}
void clearMarkdown(){
  document.clear();
  for(auto& cached:formulas){cached.begin=UINT32_MAX;cached.end=0;cached.result=math_layout::Result();}
  nextFormula=0;formulaStorage.reset();mathWorkspace.clear();formulaFailed=false;
}
bool prepareMarkdown(const char* source,size_t bytes){
  clearMarkdown();lastYield=millis();
  for(unsigned cp=0;cp<128;++cp){bodyAscii[cp]=bodyGlyphAdvance(cp);smallAscii[cp]=readerGlyphWidth(cp);bodyAsciiExists[cp]=bodyFontHasGlyph(cp);}
  // Admission and pagination use the same preallocated layout workspace and
  // eight bounded output slots. Once admitted, a formula never needs another
  // allocation while indexing or painting. Failure falls back to source text.
  if(!mathWorkspace.prepare())return false;
  formulaStorage.reset(new(std::nothrow) math_layout::Command[
    FORMULA_SLOTS*math_layout::MAX_COMMANDS]);
  if(!formulaStorage){clearMarkdown();return false;}
  return document.parse(source,bytes,cooperate,nullptr,acceptMath,nullptr)&&document.size()!=0;
}
size_t layoutMarkdown(size_t from,bool draw,int top){
  Renderer renderer;markdown_page::Geometry geometry;geometry.top=top;
  formulaFailed=false;
  const size_t next=markdown_page::page(document,from,draw,geometry,renderer,[](){cooperate(nullptr);},reader_typography::style);
  return formulaFailed?from:next;
}
