// Actual Markdown reader adapter with only time/display/body-font boundaries
// replaced. Math rasterization and pagination remain production code.
#include "markdown_reader.h"
#include "math_layout.h"
#include "markdown_samples.h"
#include "reading_font.h"
#include "reader_font.h"
#include "native_font_data.h"
#include <tuple>
#include "reader_typography.h"
namespace reader_typography {const Style style;}
#include <M5Unified.h>
#include <cassert>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <cstdio>

static int failAfter=-1;
static size_t arrayCalls=0,largestAllocation=0;
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {
  ++arrayCalls;largestAllocation=std::max(largestAllocation,bytes);
  if(failAfter==0)return nullptr;
  if(failAfter>0)--failAfter;
  return ::operator new[](bytes);
}
const reader_font::Font& bodyNativeFont(){return reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
const lgfx::IFont* bodyFont(){static lgfx::IFont font;return &font;}
const lgfx::IFont* readerFont(){return bodyFont();}
bool negativeCodeAdvance=false;
int bodyGlyphAdvance(uint32_t cp){return reader_font::glyphAdvance(reader_font::FONT_28,cp);}
const reader_font::Glyph* bodyGlyph(uint32_t cp){return reader_font::findGlyph(reader_font::FONT_28,cp);}
int readerGlyphWidth(uint32_t cp){return negativeCodeAdvance&&cp=='?'?-7:reader_font::glyphAdvance(reader_font::FONT_24,cp);}
bool bodyFontHasGlyph(uint32_t){return true;}
bool readerFontHasGlyph(uint32_t){return true;}
uint32_t readerCodepoint(const char* text,size_t bytes){return reader_font::decodeCodepoint(text,bytes);}
// Leave body text blank so framebuffer ink comes only from real math glyphs.
void drawBodyGlyph(const char*,size_t,int,int,uint16_t){}
using CodeMark=std::tuple<uint32_t,int,int>;
static std::vector<CodeMark> codeMarks;
void drawReaderGlyph(const char* text,size_t bytes,int x,int y,uint16_t){codeMarks.emplace_back(readerCodepoint(text,bytes),x,y);}

struct Page {size_t source,next,black;uint64_t hash;};
static std::vector<Page> render(const std::string& source) {
  std::vector<Page> pages;size_t offset=0;
  while(offset<source.size()) {
    const size_t measured=layoutMarkdown(offset,false);
    M5.Display.fillScreen(TFT_WHITE);
    const size_t drawn=layoutMarkdown(offset,true);
    assert(measured==drawn && drawn>offset && drawn<=source.size());
    Page page{offset,drawn,0,1469598103934665603ULL};
    for(const auto pixel:M5.Display.fb){page.black+=pixel==0;page.hash=(page.hash^pixel)*1099511628211ULL;}
    assert(page.black>0);pages.push_back(page);offset=drawn;assert(pages.size()<32);
  }
  return pages;
}
static void codeWidthRegression(){
  const std::string source="```\n  page_index = 0\n    x\n\n    y\nz\xf4\x8f\xbf\xbf" "z\n```\n";
  std::vector<CodeMark> baseline;
  assert(reader_typography::style.flags()==3);
  for(unsigned repeat=0;repeat<4;++repeat){
    assert(prepareMarkdown(source.data(),source.size()));codeMarks.clear();
    assert(layoutMarkdown(0,false)==source.size()&&layoutMarkdown(0,true)==source.size());
    if(repeat==0)baseline=codeMarks;else assert(codeMarks==baseline);
  }
  const int space=readerGlyphWidth(' ');assert(space>0&&space<24);
  bool foundP=false,foundX=false,foundY=false,foundMissing=false;int xRow=0,yRow=0;
  for(size_t i=0;i<baseline.size();++i){
    const auto cp=std::get<0>(baseline[i]);const int x=std::get<1>(baseline[i]);
    if(cp=='p'){
      foundP=true;assert(x==24+2*space);
      assert(i+1<baseline.size()&&std::get<0>(baseline[i+1])=='a');
      assert(std::get<1>(baseline[i+1])==x+readerGlyphWidth('p'));
      assert(readerGlyphWidth('p')<24); // No artificial full-width ASCII cells.
    }
    if(cp=='x'&&x==24+4*space){foundX=true;xRow=std::get<2>(baseline[i]);}
    if(cp=='y'){foundY=true;yRow=std::get<2>(baseline[i]);assert(x==24+4*space);}
    if(cp==0x10ffff){foundMissing=true;assert(i+1<baseline.size());assert(readerGlyphWidth(cp)==24&&std::get<1>(baseline[i+1])==x+24);}
  }
  assert(foundP&&foundX&&foundY&&foundMissing&&yRow>xRow+40);
  // Inline code on otherwise justified prose must preserve its own spaces.
  std::string inlineCode;for(unsigned i=0;i<12;++i)inlineCode+="words before `a b` and ordinary text after ";
  assert(prepareMarkdown(inlineCode.data(),inlineCode.size()));codeMarks.clear();
  size_t from=0;while(from<inlineCode.size()){size_t next=layoutMarkdown(from,true);assert(next>from);from=next;}
  unsigned checked=0;
  for(size_t i=0;i+2<codeMarks.size();i+=3){
    assert(std::get<0>(codeMarks[i])=='a'&&std::get<0>(codeMarks[i+1])==' '&&std::get<0>(codeMarks[i+2])=='b');
    if(std::get<2>(codeMarks[i])==std::get<2>(codeMarks[i+2])){
      assert(std::get<1>(codeMarks[i+2])-std::get<1>(codeMarks[i])==readerGlyphWidth('a')+space);++checked;
    }
  }
  assert(checked>0);
  negativeCodeAdvance=true;const std::string faulty="`?a`";
  assert(prepareMarkdown(faulty.data(),faulty.size()));codeMarks.clear();assert(layoutMarkdown(0,true)==faulty.size());
  assert(codeMarks.size()==2&&std::get<1>(codeMarks[1])==std::get<1>(codeMarks[0]));
  negativeCodeAdvance=false;clearMarkdown();
}
int main() {
  codeWidthRegression();
  const std::string sample=markdown_samples::mixed,unchanged=sample;
  for(int failure=0;failure<2;++failure) {
    clearMarkdown();failAfter=failure;arrayCalls=0;
    assert(!prepareMarkdown(sample.data(),sample.size()));
    assert(arrayCalls==size_t(failure+1));
    assert(layoutMarkdown(0,false)==0 && layoutMarkdown(0,true)==0);
    assert(sample==unchanged);  // Caller can fall back to the entire original.
  }
  failAfter=-1;arrayCalls=0;
  assert(prepareMarkdown(sample.data(),sample.size()));
  assert(arrayCalls==2 && largestAllocation==math_layout::MAX_SCRATCH_BYTES);
  clearMarkdown();

  // More than eight distinct formulae force the production cache to evict and
  // recompute slots between measuring and drawing, and again on prior pages.
  std::string formulas;
  for(unsigned n=0;n<18;++n)
    formulas+="$$\\frac{x_{"+std::to_string(n)+"}+1}{\\sqrt{y^2+2}}$$\n\n";
  assert(prepareMarkdown(formulas.data(),formulas.size()));
  const auto baseline=render(formulas);assert(baseline.size()>1);
  failAfter=0;arrayCalls=0;
  for(unsigned repeat=0;repeat<4;++repeat) {
    const auto actual=render(formulas);assert(actual.size()==baseline.size());
    for(size_t i=0;i<actual.size();++i) {
      assert(actual[i].source==baseline[i].source&&actual[i].next==baseline[i].next);
      assert(actual[i].black==baseline[i].black&&actual[i].hash==baseline[i].hash);
    }
  }
  assert(arrayCalls==0); // Even attempted late allocations would be a failure.
  clearMarkdown();failAfter=-1;
  assert(prepareMarkdown(sample.data(),sample.size()));clearMarkdown();
  std::puts("Markdown adapter admission OOM, cache eviction, allocation-free measure/draw and framebuffer equality: PASS");
}
