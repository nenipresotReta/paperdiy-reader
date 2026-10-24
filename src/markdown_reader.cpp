#include "markdown_reader.h"
#include "epub_reader.h"
#include "epub_vertical.h"
#include "epub_table.h"
#include "markdown_document.h"
#include "markdown_pagination.h"
#include "math_layout.h"
#include "math_font.h"
#include "reading_font.h"
#include "reader_font.h"
#include "reader_work.h"
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
void cooperate(void* context){
  static_cast<reader_work::Budget*>(context)->cooperate([](){return millis();},[](){delay(1);});
}
bool mathCooperate(void* context){cooperate(context);return true;}
math_layout::Options options(bool display,reader_work::Budget& budget){
  math_layout::Options value;value.displayStyle=display;value.maxWidth=reader_settings::geometry().right-reader_settings::geometry().left;
  value.maxHeight=840;value.keepGoing=mathCooperate;value.context=&budget;return value;
}
bool acceptMath(const char* source,size_t bytes,bool display,void* context){
  return math_layout::layoutInto(source,bytes,options(display,*static_cast<reader_work::Budget*>(context)),mathWorkspace,
    slot(0),math_layout::MAX_COMMANDS).ok();
}
struct CachedFormula {
  uint32_t begin=UINT32_MAX,end=0;const char* source=nullptr;
  math_layout::Result result;
};
CachedFormula formulas[FORMULA_SLOTS];
unsigned nextFormula=0;
bool epubMathAdmitted[epub_objects::MAX_MATH]{};
markdown::Run epubMathFallback[epub_objects::MAX_MATH];
const math_layout::Result& formula(const markdown::Run& run,reader_work::Budget& budget,const epub_document::Document* epub=nullptr){
  const size_t indexInEpub=epub?epub->mathAt(run.begin):0;
  const char* source=epub?epub->mathText(indexInEpub):document.source()+run.formulaBegin;
  const size_t bytes=epub?epub->math(indexInEpub).texBytes:run.formulaEnd-run.formulaBegin;
  for(const auto& cached:formulas)
    if(cached.begin==run.formulaBegin&&cached.end==run.formulaEnd&&cached.source==source)return cached.result;
  const unsigned index=nextFormula++%FORMULA_SLOTS;
  auto& cached=formulas[index];
  cached.begin=run.formulaBegin;cached.end=run.formulaEnd;cached.source=source;
  cached.result=math_layout::layoutInto(source,bytes,options(run.kind==markdown::Kind::MathBlock,budget),
    mathWorkspace,slot(index),math_layout::MAX_COMMANDS);
  return cached.result;
}
unsigned emphasisPixels(){return reader_settings::geometry().pixels==24?20:28;}
uint32_t alternateGlyph(const markdown::Run& run,uint32_t cp){
  if((run.flags&markdown::Emphasis)&&!(run.flags&markdown::Code)&&
      ((cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z'))){
    uint32_t styled=math_font::styled(cp,math_font::Style::Italic);
    if(math_font::find(styled,emphasisPixels()))return styled;
  }
  if(!(run.flags&markdown::Code)&&!(cp<128?bodyAsciiExists[cp]:bodyFontHasGlyph(cp))&&math_font::find(cp,emphasisPixels()))return cp;
  return 0;
}
struct Renderer {
  reader_work::Budget& budget;const epub_document::Document* epub=nullptr;
  explicit Renderer(reader_work::Budget& work,const epub_document::Document* source=nullptr):budget(work),epub(source){}
  markdown_page::Box glyph(const markdown::Run& run,const char* ch,size_t bytes){
    uint32_t cp=readerCodepoint(ch,bytes);markdown_page::Box box;
    const bool code=(run.flags&markdown::Code)!=0;
    box.ascent=code?26:bodyFontBaseline();box.descent=code?8:bodyFontHeight()-bodyFontBaseline();
    const uint32_t emphasis=alternateGlyph(run,cp);
    if(emphasis){
      auto g=math_font::find(emphasis,emphasisPixels());box.width=g->advance+std::max<int>(0,g->italicCorrection);
      box.ascent=std::max<int>(box.ascent,-g->top);
      box.descent=std::max<int>(box.descent,g->top+g->height);
      box.ink=g->width&&g->height;box.inkLeft=g->left;box.inkRight=g->left+g->width;
    }else box.width=code?(cp<32?0:std::max(0,cp<128?smallAscii[cp]:readerGlyphWidth(cp))):(cp<128?bodyAscii[cp]:bodyGlyphAdvance(cp));
    if(!emphasis&&!code&&typography_experiment::punctuation(cp)!=typography_experiment::Punctuation::None){
      if(const auto* g=bodyGlyph(cp)){box.ink=g->width&&g->height;box.inkLeft=g->left;box.inkRight=g->left+g->width;}
    }
    if(!emphasis&&!code)box.nativePixels=reader_settings::geometry().pixels;
    if(box.ink&&(run.flags&(markdown::Strong|markdown::Heading)))++box.inkRight;
    return box;
  }
  markdown_page::Box math(const markdown::Run& run){
    const auto& value=formula(run,budget,epub);markdown_page::Box box;
    if(!value.ok())formulaFailed=true;
    box.width=value.width;box.ascent=value.ascent;box.descent=value.descent;return box;
  }
  void rule(int x,int y,int width,int height){M5.Display.fillRect(x,y,width,height,TFT_BLACK);}
  void drawMath(const markdown::Run& run,int x,int baseline){
    const auto& value=formula(run,budget,epub);
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
      if(emphasis)math_font::drawGlyphRuns(emphasis,emphasisPixels(),left,baseline,
        [](int xx,int yy,int width){M5.Display.drawFastHLine(xx,yy,width,TFT_BLACK);});
      else if(code)drawReaderGlyph(ch,bytes,left,baseline-26);
      else drawBodyGlyph(ch,bytes,left,baseline-bodyFontBaseline());
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
  clearMarkdown();reader_work::Budget budget(millis(),reader_work::INDEX_INTERVAL_MS);
  for(unsigned cp=0;cp<128;++cp){bodyAscii[cp]=bodyGlyphAdvance(cp);smallAscii[cp]=readerGlyphWidth(cp);bodyAsciiExists[cp]=bodyFontHasGlyph(cp);}
  // Admission and pagination use the same preallocated layout workspace and
  // eight bounded output slots. Once admitted, a formula never needs another
  // allocation while indexing or painting. Failure falls back to source text.
  if(!mathWorkspace.prepare())return false;
  formulaStorage.reset(new(std::nothrow) math_layout::Command[
    FORMULA_SLOTS*math_layout::MAX_COMMANDS]);
  if(!formulaStorage){clearMarkdown();return false;}
  return document.parse(source,bytes,cooperate,&budget,acceptMath,&budget)&&document.size()!=0;
}
size_t layoutMarkdown(size_t from,bool draw,int top,reader_work::Budget* sharedBudget){
  reader_work::Budget localBudget(millis(),draw?reader_work::DRAW_INTERVAL_MS:reader_work::INDEX_INTERVAL_MS);
  auto& budget=sharedBudget?*sharedBudget:localBudget;
  Renderer renderer{budget};markdown_page::Geometry geometry;geometry.top=top;geometry.advanced=true;
  const auto shape=reader_settings::geometry();geometry.left=shape.left;geometry.right=shape.right;geometry.lineHeight=shape.lineHeight;
  geometry.hyphenate=reader_settings::current().hyphenation;geometry.justify=reader_settings::current().justify;
  formulaFailed=false;
  const size_t next=markdown_page::page(document,from,draw,geometry,renderer,[&](){cooperate(&budget);},reader_typography::currentStyle());
  return formulaFailed?from:next;
}

// EPUB and Markdown share the native glyph/inline-style renderer. EPUB keeps
// identity source offsets; only admitted MathML books prepare math storage.
void prepareEpubMetrics(){
  for(unsigned cp=0;cp<128;++cp){bodyAscii[cp]=bodyGlyphAdvance(cp);smallAscii[cp]=readerGlyphWidth(cp);bodyAsciiExists[cp]=bodyFontHasGlyph(cp);}
  for(auto& cached:formulas){cached.begin=UINT32_MAX;cached.source=nullptr;cached.result=math_layout::Result();}nextFormula=0;
  std::fill(std::begin(epubMathAdmitted),std::end(epubMathAdmitted),false);
  if(!bookDocument.mathCount())return;
  const bool workspace=mathWorkspace.prepare();
  if(!formulaStorage)formulaStorage.reset(new(std::nothrow) math_layout::Command[FORMULA_SLOTS*math_layout::MAX_COMMANDS]);
  if(!workspace||!formulaStorage)bookEpubTransientFallback=true;
  reader_work::Budget budget(millis(),reader_work::INDEX_INTERVAL_MS);
  for(size_t i=0;i<bookDocument.mathCount();++i){const auto& m=bookDocument.math(i);auto& fallback=epubMathFallback[i];fallback=bookDocument.run(bookDocument.runAt(m.begin));fallback.kind=markdown::Kind::Text;
    if(workspace&&formulaStorage)epubMathAdmitted[i]=math_layout::layoutInto(bookDocument.mathText(i),m.texBytes,options(m.flags&epub_objects::MATH_DISPLAY,budget),mathWorkspace,slot(0),math_layout::MAX_COMMANDS).ok();
  }
}
size_t layoutEpub(size_t from,bool draw,int top,reader_work::Budget* sharedBudget){
  reader_work::Budget localBudget(millis(),draw?reader_work::DRAW_INTERVAL_MS:reader_work::INDEX_INTERVAL_MS);
  auto& budget=sharedBudget?*sharedBudget:localBudget;
  const auto shape=reader_settings::geometry();
  if(bookDocument.verticalAt(from)){
    struct VerticalRenderer {
      int advance(uint32_t cp){int advance=bodyGlyphAdvance(cp);if(const auto* g=bodyGlyph(cp))advance=std::max(advance,int(g->width)+std::max(0,int(g->left)));return advance;}
      void glyph(uint32_t cp,unsigned pixels,int x,int y,bool sideways,size_t,size_t){
        vertical_font::draw(bodyNativeFont(),cp,pixels,x,y,sideways,[](int left,int top,int width,int height){M5.Display.fillRect(left,top,width,height,TFT_BLACK);});
      }
    } renderer;
    epub_vertical::Geometry geometry;geometry.left=shape.left;geometry.right=shape.right;geometry.top=top;
    geometry.pixels=shape.pixels;geometry.columnPitch=shape.lineHeight;
    return epub_vertical::page(bookDocument,from,draw,geometry,renderer,[&](){cooperate(&budget);});
  }
  // Piecewise horizontal flow keeps the canonical cursor and the consumed
  // vertical coordinate together. Whole table rows may use the remaining page;
  // a row that does not fit is deferred without consuming its source span.
  struct HorizontalView {
    const epub_document::Document& doc;size_t end;
    size_t size()const{return end;}size_t sourceSize()const{return end;}
    const char* text()const{return doc.text();}size_t runCount()const{return doc.runCount();}
    size_t runAt(size_t at)const{return doc.runAt(at);}const markdown::Run& run(size_t i)const{const auto& r=doc.run(i);if(r.kind==markdown::Kind::MathInline||r.kind==markdown::Kind::MathBlock){size_t m=doc.mathAt(r.begin);if(m<doc.mathCount()&&!epubMathAdmitted[m])return epubMathFallback[m];}return r;}
    size_t renderOffset(size_t at)const{return std::min(at,end);}size_t sourceOffset(size_t at)const{return std::min(at,end);}
  };
  const size_t limit=std::min(bookDocument.modeEnd(from),readerContentLimit());size_t cursor=from;int y=top;
  Renderer renderer{budget,&bookDocument};
  while(cursor<limit&&y<916){
    size_t proseEnd=std::min(limit,bookDocument.nextTable(cursor));const size_t tableIndex=bookDocument.tableAt(cursor);
    if(tableIndex<bookDocument.tableCount()){
      const auto& table=bookDocument.table(tableIndex);epub_table::Geometry geometry;geometry.left=shape.left;geometry.right=shape.right;geometry.top=y;geometry.pageTop=top;geometry.lineHeight=shape.lineHeight;
      const auto result=epub_table::page(bookDocument,cursor,draw,geometry,renderer,[&](){cooperate(&budget);});
      if(!result.fallback){
        if(result.next<=cursor)return cursor;cursor=result.next;y=result.bottom;
        if(cursor<table.end)return cursor;
        for(unsigned skipped=0;skipped<2&&cursor<limit&&(bookDocument.text()[cursor]=='\n'||bookDocument.text()[cursor]=='\r');++skipped)++cursor;
        continue;
      }
      proseEnd=std::min(limit,result.fallbackEnd);
    }
    HorizontalView view{bookDocument,proseEnd};markdown_page::Geometry geometry;geometry.top=y;geometry.advanced=true;
    geometry.left=shape.left;geometry.right=shape.right;geometry.lineHeight=shape.lineHeight;
    geometry.hyphenate=reader_settings::current().hyphenation;geometry.justify=reader_settings::current().justify;
    int bottom=y;const size_t next=markdown_page::page(view,cursor,draw,geometry,renderer,[&](){cooperate(&budget);},reader_typography::currentStyle(),&bottom);
    if(next<=cursor)return cursor;cursor=next;y=bottom;if(cursor<proseEnd)return cursor;
  }
  return cursor;
}
