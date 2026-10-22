#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "pagination.h"
#include "native_font_data.h"

struct Glyph { std::string text; int x, y, w; bool inserted; };
using Rows = std::map<int,std::vector<Glyph>>;
int fixed(const char* p,size_t n) { return pagination::combining(pagination::codepoint(p,n)) ? 0 : n==1 ? (*p==' ' ? 2 : 4) : 8; }
pagination::Geometry geometry(int w,int rows=1) {
  pagination::Geometry g;g.left=0;g.right=w;g.top=0;g.bottom=(rows-1)*10;g.lineHeight=10;
  return g;
}
Rows render(const std::string& text,pagination::Geometry g,size_t from=0,size_t* end=nullptr) {
  Rows rows;
  auto draw=[&](const char* s,size_t n,int x,int y){
    bool inserted = uintptr_t(s)<uintptr_t(text.data()) || uintptr_t(s)>=uintptr_t(text.data()+text.size());
    rows[y].push_back({std::string(s,n),x,y,fixed(s,n),inserted});
  };
  size_t p=pagination::page(text.data(),text.size(),from,g,fixed,draw,[](){});
  if(end)*end=p;
  for(const auto& row:rows){
    const auto& glyphs=row.second;
    for(size_t i=0;i<glyphs.size();++i){
      const auto& gl=glyphs[i];
      bool hanging=i+1==glyphs.size() &&
        pagination::closing(pagination::codepoint(gl.text.data(),gl.text.size()));
      assert(gl.x>=g.left && (gl.x+gl.w<=g.right || hanging || gl.w>g.right-g.left));
    }
  }
  return rows;
}
std::string content(const std::vector<Glyph>& row) {std::string out;for(const auto& g:row)out+=g.text;return out;}
std::string split(const std::string& word) {
  uint64_t mask=english_hyphenation::points(word.data(),word.size());std::string out;
  for(size_t i=0;i<word.size();++i){if(mask&(1ULL<<i))out+='-';out+=word[i];}return out;
}
void basic() {
  assert(split("hyphenation")=="hy-phen-ation");
  assert(split("representation")=="rep-re-sen-ta-tion");
  assert(split("associate")=="as-so-ciate");
  assert(split("project")=="project");
  assert(!english_hyphenation::points("HTTP",4));
  assert(!english_hyphenation::points("https://example.com",19));
  assert(!english_hyphenation::points("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",52));
  auto g=geometry(28,2);g.hyphenate=false;size_t end=0;
  auto rows=render("one hello end",g,0,&end);
  assert(content(rows.at(0))=="one" && content(rows.at(10))=="hello");
  assert(end==10); // The next page begins at 'end', not an invisible space.
  auto r=pagination::line("one hello",9,0,g,fixed,[](){});
  assert(r.end==3 && r.next==4 && !r.hyphen && !r.paragraphEnd);
  g=geometry(43,1);g.hyphenate=true;
  std::string sentence="a representation";
  auto hyph=pagination::line(sentence.data(),sentence.size(),0,g,fixed,[](){});
  assert(hyph.hyphen && hyph.advance<=43 && hyph.end>2);
  rows=render(sentence,g);assert(content(rows.at(0)).back()=='-');

  // Justify short gaps, leave paragraph-final and overly sparse lines alone.
  g=geometry(30,2);g.hyphenate=false;
  rows=render("aa bb cc dd",g);
  assert(rows.at(0).back().x+rows.at(0).back().w==30);
  assert(rows.at(10).front().x==0);
  g=geometry(30,1);g.hyphenate=false;
  rows=render("aa bb\n",g);assert(rows.at(0).back().x+4==18);
  rows=render("a extraordinarily",geometry(36));
  auto noAuto=geometry(36);noAuto.hyphenate=false;
  rows=render("a extraordinarily",noAuto);assert(content(rows.at(0))=="a");
  assert(rows.at(0).back().x+4==4);

  // Hanging punctuation: a closing mark that would overflow stays on the line.
  g=geometry(56);g.hyphenate=true;
  auto hanging=pagination::line("representation.",15,0,g,fixed,[](){});
  assert(hanging.hang==4 && hanging.end==15 && hanging.next==15);
  assert(hanging.advance==56 && !hanging.hyphen);
  auto trailing=pagination::line("representation,",15,0,g,fixed,[](){});
  assert(trailing.hang==4 && trailing.end==15 && !trailing.hyphen);
  // A margin budget can refuse a hang that would leave the reserved zone.
  g.hangLimit=3;
  auto capped=pagination::line("representation.",15,0,g,fixed,[](){});
  assert(capped.hang==0 && capped.end<15);
  g.hangLimit=4;
  auto withinBudget=pagination::line("representation.",15,0,g,fixed,[](){});
  assert(withinBudget.hang==4 && withinBudget.end==15);
  // Full-width CJK punctuation uses 8 px at the test metrics; cap it too.
  g=geometry(16);g.hyphenate=false;g.hangLimit=8;
  auto cjkHang=pagination::line("中文。",9,0,g,fixed,[](){});
  assert(cjkHang.hang==8 && cjkHang.end==9 && cjkHang.advance==16);
  g.hangLimit=7;
  auto cjkCapped=pagination::line("中文。",9,0,g,fixed,[](){});
  assert(cjkCapped.hang==0);
  // A word that does not fit may still break before its sentence-final period.
  g=geometry(40);g.hyphenate=true;
  auto splitBeforePeriod=pagination::line("representation.",15,0,g,fixed,[](){});
  assert(splitBeforePeriod.hyphen && splitBeforePeriod.end<14 && !splitBeforePeriod.hang);
  g=geometry(56);g.hyphenate=false;
  std::vector<int> periodX;
  std::string periodText;
  size_t hangEnd=pagination::page("representation.",15,0,g,fixed,
    [&](const char*s,size_t n,int x,int y){(void)y;periodX.push_back(x);periodText.append(s,n);},[](){});
  assert(hangEnd==15 && periodText=="representation.");
  assert(periodX.size()==15 && periodX.back()==56);

  // The last line of a multi-line page is not hyphenated; it moves instead.
  g=geometry(20,2);g.hyphenate=true;
  std::string pageText="aa\nrepresentation";
  std::map<int,std::string> pageLines;
  size_t moved=pagination::page(pageText.data(),pageText.size(),0,g,fixed,
    [&](const char*s,size_t n,int x,int y){(void)x;pageLines[y].append(s,n);},[](){});
  assert(moved==3 && pageLines.size()==1 && pageLines.begin()->second=="aa");
  bool hyphenOnNextPage=false;
  size_t nextPage=pagination::page(pageText.data(),pageText.size(),moved,g,fixed,
    [&](const char*s,size_t n,int,int){if(n==1&&s[0]=='-')hyphenOnNextPage=true;},[](){});
  assert(nextPage>moved && hyphenOnNextPage);

  // Soft hyphens are invisible except at an actual permitted break.
  std::string shy="co\xc2\xadoperate";
  g=geometry(60);g.hyphenate=false;
  rows=render(shy,g);assert(content(rows.at(0))=="cooperate");
  g=geometry(16);g.hyphenate=false;
  rows=render(shy,g);assert(content(rows.at(0))=="co-");
  rows=render("co\xc2\xadop\xc2\xad" "eration",g);assert(content(rows.at(0))=="co-");
  assert(!pagination::softPoint("a\xc2\xad" "b",4,1));
  std::string glued="xx a\xc2\xa0" "b";
  g=geometry(18,2);g.hyphenate=false;
  rows=render(glued,g);assert(content(rows.at(0))=="xx" && content(rows.at(10))=="a b");
  rows=render("don't stop",geometry(24,2));assert(content(rows.at(0))=="don't");
  rows=render("well-known",geometry(24,2));assert(content(rows.at(0))=="well-");
  rows=render("https://example.com/abcdefghijklmnop",geometry(28,8));
  for(const auto& row:rows)for(const auto& glyph:row.second)
    assert(!(glyph.inserted && glyph.text=="-"));
  rows=render("a  bb cc dd",geometry(32,2));
  assert(rows.at(0).back().x+rows.at(0).back().w<32); // Preformatted spaces preserved.
  g=geometry(28,2);g.hyphenate=false;
  rows=render("aa bb   \ncc",g);assert(content(rows.at(0))=="aa bb" && content(rows.at(10))=="cc");
  rows=render("中文，中文。",geometry(24,4));
  for(const auto& row:rows) assert(row.second.front().text!="，" && row.second.front().text!="。");
  std::string accents="ab e\xcc\x81" "fg hi";
  rows=render(accents,geometry(20,5));
  for(const auto& row:rows)assert(row.second.front().text!="\xcc\x81");
}
void paginationRegression() {
  std::string text;
  for(int i=0;i<100;++i)text+="A representation of extraordinary navigation, with co\xc2\xadoperation and don't, 3.14159, well-known words. 中文段落。\n\n";
  auto g=geometry(72,4);std::vector<uint32_t> offsets;
  auto next=[&](size_t p){return pagination::page(text.data(),text.size(),p,g,fixed,[](const char*,size_t,int,int){},[](){});};
  assert(pagination::index(text.size(),offsets,next));
  unsigned globalStreak=0;std::string visible;
  for(size_t i=0;i<offsets.size();++i) {
    assert((uint8_t(text[offsets[i]])&0xc0)!=0x80);
    size_t end=0;auto rows=render(text,g,offsets[i],&end);
    assert(end==(i+1<offsets.size()?offsets[i+1]:text.size()));
    // No more than two consecutive discretionary hyphens on a page.
    unsigned streak=0;
    for(const auto& row:rows){
      const auto& last=row.second.back();
      streak=last.inserted && last.text=="-" ? streak+1 : 0;assert(streak<=2);
      globalStreak=last.inserted && last.text=="-" ? globalStreak+1 : 0;assert(globalStreak<=2);
      for(const auto& glyph:row.second)if(!glyph.inserted && glyph.text!=" ")visible+=glyph.text;
    }
  }
  std::string expected;
  for(size_t p=0;p<text.size();){
    size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);
    uint32_t cp=pagination::codepoint(text.data()+p,n);
    if(cp!='\n'&&!pagination::space(cp)&&!pagination::noBreakSpace(cp)&&!pagination::invisible(cp))expected.append(text,p,n);
    p+=n;
  }
  assert(visible==expected); // Inserted hyphens never consume or duplicate book bytes.
  std::string enormous(100000,'x');
  assert(pagination::index(enormous.size(),offsets,[&](size_t p){
    return pagination::page(enormous.data(),enormous.size(),p,g,fixed,[](const char*,size_t,int,int){},[](){});
  }));
  assert(offsets.size()<65536);
  // Zero-width input remains bounded and advances, including an overwide glyph.
  std::string zeros;for(int i=0;i<10000;++i)zeros+="\xe2\x81\xa0";
  assert(pagination::page(zeros.data(),zeros.size(),0,g,fixed,[](const char*,size_t,int,int){},[](){})>0);
}
void readerGeometryRegression() {
  std::string text;
  for(int i=0;i<200;++i)
    text+="A representation of extraordinary navigation, with co\xc2\xadoperation and don't, 3.14159, well-known words. 中文段落，测试。\n\n";
  pagination::Geometry g;
  g.left=24;g.right=510;g.top=24;g.bottom=886;g.lineHeight=40;g.hangLimit=30;
  auto width=[](const char* p,size_t n){
    return reader_font::glyphAdvance(reader_font::FONT_28,pagination::codepoint(p,n));
  };
  std::vector<uint32_t> offsets;
  auto next=[&](size_t p){
    return pagination::page(text.data(),text.size(),p,g,width,
      [](const char*,size_t,int,int){},[](){});
  };
  assert(pagination::index(text.size(),offsets,next));
  std::string visible;
  for(size_t i=0;i<offsets.size();++i){
    bool hyphenAtBottom=false;
    size_t end=pagination::page(text.data(),text.size(),offsets[i],g,width,
      [&](const char* s,size_t n,int,int y){
        const bool inserted=uintptr_t(s)<uintptr_t(text.data())||
          uintptr_t(s)>=uintptr_t(text.data()+text.size());
        if(inserted){
          if(n==1&&s[0]=='-'&&y==g.bottom)hyphenAtBottom=true;
          return;
        }
        const uint32_t cp=pagination::codepoint(s,n);
        if(!pagination::space(cp)&&!pagination::noBreakSpace(cp)&&!pagination::invisible(cp))
          visible.append(s,n);
      },[](){});
    assert(end>offsets[i]&&end<=text.size());
    assert(end==(i+1<offsets.size()?offsets[i+1]:text.size()));
    assert(!hyphenAtBottom);
  }
  std::string expected;
  for(size_t p=0;p<text.size();){
    size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);
    uint32_t cp=pagination::codepoint(text.data()+p,n);
    if(cp!='\n'&&!pagination::space(cp)&&!pagination::noBreakSpace(cp)&&!pagination::invisible(cp))
      expected.append(text,p,n);
    p+=n;
  }
  assert(visible==expected);
}
void visual(const char* directory) {
  std::string text="Careful typography makes a long passage easier to read. Each line should preserve the shape of its words while the spaces remain calm and consistent. A representation of extraordinary circumstances may need a discretionary hyphen, but a short final line should rest naturally at the left margin.\n\n"
    "When the reader changes the layout, the book itself remains untouched. Navigation remembers a position in the original text. This distinction keeps a passage accessible even when a different arrangement produces a different number of pages.\n\n"
    "An international conversation about mountaineering, exploration, and responsibility illustrates the relationship between automatic hyphenation and comfortable word spacing. The compositor considers the available width before deciding where the next line begins.\n\n"
    "A final short paragraph.";
  pagination::Geometry g;g.right=510;g.hangLimit=30;
  std::vector<uint8_t> canvas(540*960,255);
  auto width=[](const char* p,size_t n){return reader_font::glyphAdvance(reader_font::FONT_28,pagination::codepoint(p,n));};
  auto draw=[&](const char* p,size_t n,int x,int y){
    reader_font::drawGlyphRuns(reader_font::FONT_28,pagination::codepoint(p,n),x,y,[&](int left,int top,int count){
      assert(top>=0 && top<960 && left>=0 && left+count<=540);
      for(int i=0;i<count;++i)canvas[top*540+left+i]=0;
    });
  };
  size_t end=pagination::page(text.data(),text.size(),0,g,width,draw,[](){});
  std::ofstream f(std::string(directory)+"/english-layout.pgm",std::ios::binary);
  f<<"P5\n540 960\n255\n";f.write((char*)canvas.data(),canvas.size());
  printf("visual bytes=%zu/%zu\n",end,text.size());
}
int main(int argc,char** argv) {
  basic();paginationRegression();readerGeometryRegression();
  if(argc>1)visual(argv[1]);
  puts("word wrapping, Liang patterns, bounded justification, controls, mixed text, cached/drawn boundaries: PASS");
}
