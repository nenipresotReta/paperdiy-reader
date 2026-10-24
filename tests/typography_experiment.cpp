#include "experimental_typography.h"
#include "native_font_data.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <tuple>
#include <cstring>
using Options=typography_experiment::Options;
using Metrics=typography_experiment::Metrics;
Metrics metrics(uint32_t cp){
  Metrics m;m.advance=reader_font::glyphAdvance(reader_font::FONT_28,cp);
  if(const auto* g=reader_font::findGlyph(reader_font::FONT_28,cp)){
    m.ink=g->width&&g->height;m.inkLeft=g->left;m.inkRight=g->left+g->width;
  }
  return m;
}
int nativeWidth(const char* p,size_t n){return metrics(pagination::codepoint(p,n)).advance;}
using Mark=std::tuple<std::string,int,int>;
struct Render {size_t next=0;std::vector<Mark> marks;std::string content;};
Render render(const std::string& text,size_t from,pagination::Geometry g,Options options,bool legacy=false){
  Render result;
  auto draw=[&](const char* p,size_t n,int x,int y){
    result.marks.emplace_back(std::string(p,n),x,y);
    uint32_t cp=pagination::codepoint(p,n);
    const uintptr_t begin=reinterpret_cast<uintptr_t>(text.data()),at=reinterpret_cast<uintptr_t>(p);
    if(at>=begin&&at<begin+text.size()&&!pagination::space(cp)&&!pagination::noBreakSpace(cp))result.content.append(p,n);
  };
  result.next=legacy?pagination::page(text.data(),text.size(),from,g,nativeWidth,draw,[](){}):
    typography_experiment::page(text.data(),text.size(),from,g,options,metrics,draw,[](){});
  return result;
}
std::string visible(const std::string& text){
  std::string result;
  for(size_t p=0;p<text.size();){size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);uint32_t cp=pagination::codepoint(text.data()+p,n);
    if(!pagination::space(cp)&&!pagination::noBreakSpace(cp)&&!pagination::invisible(cp)&&cp!='\n')result.append(text,p,n);p+=n;}
  return result;
}
pagination::Geometry geometry(){pagination::Geometry g;g.left=24;g.right=510;g.top=24;g.bottom=886;g.hangLimit=30;return g;}
void writeImage(const char* filename,const Render& rendered){
  std::vector<uint8_t> pixels(540*960,255);
  for(const auto& mark:rendered.marks){
    uint32_t cp=pagination::codepoint(std::get<0>(mark).data(),std::get<0>(mark).size());
    reader_font::drawGlyphRuns(reader_font::FONT_28,cp,std::get<1>(mark),std::get<2>(mark),
      [&](int x,int y,int count){
        assert(y>=20&&y<926&&x>=20&&x+count<=540);
        for(int i=0;i<count;++i)pixels[y*540+x+i]=0;
      });
  }
  FILE* f=fopen(filename,"wb");assert(f);fprintf(f,"P5\n540 960\n255\n");
  assert(fwrite(pixels.data(),1,pixels.size(),f)==pixels.size());fclose(f);
}
int main(){
  Options off,compact,gap,both;compact.compressAdjacentPunctuation=true;gap.hanLatinGap=4;both=compact;both.hanLatinGap=4;
  const std::string punctuation="《「正文」》、（“内容”）。！？……——";
  unsigned trimmed=0;
  for(size_t p=0;p<punctuation.size();){
    const size_t n=pagination::utf8Bytes(punctuation.data()+p,punctuation.size()-p);
    int delta=typography_experiment::leading(punctuation.data(),punctuation.size(),p,0,compact,metrics);
    assert(delta>=-14&&delta<=0);
    if(delta){
      ++trimmed;auto a=metrics(pagination::previous(punctuation.data(),p));
      auto b=metrics(pagination::codepoint(punctuation.data()+p,n));
      assert(a.advance+delta+b.inkLeft-a.inkRight>=2&&b.advance+delta>=1);
    }
    assert(typography_experiment::leading(punctuation.data(),punctuation.size(),p,p,compact,metrics)==0);
    p+=n;
  }
  assert(trimmed>=5);
  for(const char* sample:{"……","——","...","()","：文字","文字。"}){
    std::string text=sample;
    for(size_t p=0;p<text.size();){size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);
      assert(typography_experiment::leading(text.data(),text.size(),p,0,compact,metrics)==0);p+=n;}
  }
  for(const char* sample:{"中文 EPUB3 中文","中文（EPUB3）中文","中文\xc2\xa0" "EPUB3","中\xe2\x81\xa0" "A","中\xef\xb8\x80" "A"}){
    std::string text=sample;
    for(size_t p=0;p<text.size();){size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);
      assert(typography_experiment::leading(text.data(),text.size(),p,0,gap,metrics)==0);p+=n;}
  }
  std::string mixed="中文EPUB3中文";unsigned gaps=0;
  for(size_t p=0;p<mixed.size();){size_t n=pagination::utf8Bytes(mixed.data()+p,mixed.size()-p);
    if(typography_experiment::leading(mixed.data(),mixed.size(),p,0,gap,metrics)==4)++gaps;p+=n;}
  assert(gaps==2);
  const std::vector<std::string> samples={
    "她说：“《春日》很好。”他回答：“真的吗？！”随后，读者打开EPUB3文件，在第12章看见LaTeX公式。",
    "中文 EPUB3 中文\n\n中文（EPUB3）中文\n\nEnglish representation and hyphenation with ordinary words. ",
    "《「正文」》（“正文”），？！……未完的句子——还有下一段。\n",
    "中\xc2\xa0文 e\xcc\x81 中文，soft\xc2\xadware and under_score@example.com\n",
  };
  for(unsigned width:{120u,251u,485u,486u})for(const auto& base:samples){
    std::string text;for(unsigned i=0;i<24;++i)text+=base;
    auto g=geometry();g.right=g.left+int(width);
    size_t from=0;unsigned pages=0;
    while(from<text.size()){
      auto a=render(text,from,g,off,true),b=render(text,from,g,off);
      assert(a.next==b.next&&a.marks==b.marks&&b.next>from);
      from=b.next;assert(++pages<200);
    }
    for(auto options:{compact,gap,both}){
      from=0;pages=0;std::string painted;
      while(from<text.size()){
        auto a=render(text,from,g,options),b=render(text,from,g,options);
        assert(a.next==b.next&&a.marks==b.marks&&a.next>from&&a.next<=text.size());
        assert(a.next==text.size()||(uint8_t(text[a.next])&0xc0)!=0x80);
        for(const auto& mark:a.marks){assert(std::get<1>(mark)>=g.left&&std::get<2>(mark)<=g.bottom);}
        painted+=a.content;from=a.next;assert(++pages<200);
      }
      assert(painted==visible(text));
    }
  }
  // Index-only work must choose exactly the rendered boundaries, never call
  // the pixel callback, and avoid the second per-glyph measurement pass.
  size_t indexedMeasurements=0,paintedMeasurements=0;
  for(auto options:{off,compact,gap,both})for(const auto& base:samples){
    std::string text;for(unsigned i=0;i<16;++i)text+=base;
    auto g=geometry();size_t from=0;
    while(from<text.size()){
      auto indexed=[&](uint32_t cp){++indexedMeasurements;return metrics(cp);};
      auto painted=[&](uint32_t cp){++paintedMeasurements;return metrics(cp);};
      const size_t a=typography_experiment::page(text.data(),text.size(),from,g,options,indexed,
        [](const char*,size_t,int,int){assert(false);},[](){},false);
      const size_t b=typography_experiment::page(text.data(),text.size(),from,g,options,painted,
        [](const char*,size_t,int,int){},[](){});
      assert(a==b&&a>from);from=a;
    }
  }
  assert(indexedMeasurements*100<paintedMeasurements*70);
  printf("Index-only measurement work: %zu / %zu (%.1f%%)\n",indexedMeasurements,
    paintedMeasurements,100.0*indexedMeasurements/paintedMeasurements);
  // Production-quality metrics and bounded line choice share exact source
  // decisions in the cached and painted paths, for both native font sizes.
  for(unsigned pixels:{24u,28u}){
    auto advanced=both;advanced.latinPixels=pixels;advanced.balanceLines=true;
    advanced.strictBreaks=true;advanced.opticalMargin=4;
    const auto& font=pixels==24?reader_font::FONT_24:reader_font::FONT_28;
    auto measuredFont=[&](uint32_t cp){
      Metrics m;m.advance=reader_font::glyphAdvance(font,cp);
      if(const auto* glyph=reader_font::findGlyph(font,cp)){
        m.ink=glyph->width&&glyph->height;m.inkLeft=glyph->left;m.inkRight=glyph->left+glyph->width;
      }
      return m;
    };
    std::string text;
    for(unsigned i=0;i<80;++i)text+=samples[i%samples.size()];
    auto g=geometry();size_t from=0;std::string painted;
    while(from<text.size()){
      auto drawer=[&](const char* ch,size_t n,int x,int y){
        const auto address=reinterpret_cast<uintptr_t>(ch),begin=reinterpret_cast<uintptr_t>(text.data());
        assert(x>=g.left-4&&x<=540&&y<=g.bottom);
        if(address>=begin&&address<begin+text.size()){
          auto cp=pagination::codepoint(ch,n);
          if(!pagination::space(cp)&&!pagination::noBreakSpace(cp))painted.append(ch,n);
        }
      };
      const auto indexed=typography_experiment::page(text.data(),text.size(),from,g,advanced,
        measuredFont,[](const char*,size_t,int,int){assert(false);},[](){},false);
      const auto rendered=typography_experiment::page(text.data(),text.size(),from,g,advanced,
        measuredFont,drawer,[](){});
      assert(indexed==rendered&&indexed>from);from=indexed;
    }
    assert(painted==visible(text));
  }
  // Bound the aggregate cost on an ordinary book-sized paragraph stream.
  // This checks measured work, not machine-dependent wall-clock timing.
  {
    std::string text;
    for(unsigned i=0;i<100;++i)text+="Careful typography makes a long passage easier to read. Each line should preserve the shape of its words while the spaces remain calm and consistent. A representation of extraordinary circumstances may need a discretionary hyphen, but a short final line should rest naturally at the left margin.\n\n";
    size_t work[2]={};
    for(unsigned mode=0;mode<2;++mode){
      auto options=both;options.latinPixels=28;options.strictBreaks=true;options.opticalMargin=4;
      options.balanceLines=mode!=0;
      size_t from=0;
      auto counted=[&](uint32_t cp){++work[mode];return metrics(cp);};
      while(from<text.size()){
        const auto next=typography_experiment::page(text.data(),text.size(),from,geometry(),options,
          counted,[](const char*,size_t,int,int){assert(false);},[](){},false);
        assert(next>from);from=next;
      }
    }
    assert(work[1]*100<=work[0]*150);
    printf("Bounded line-quality measurement work: %zu / %zu (%.1f%%)\n",work[1],work[0],100.0*work[1]/work[0]);
  }
  // Invalid geometry behaves like the production path and never draws.
  for(unsigned kind=0;kind<3;++kind){
    auto g=geometry();
    if(kind==0)g.right=g.left;
    if(kind==1)g.lineHeight=0;
    if(kind==2)g.bottom=g.top-1;
    auto a=render(mixed,3,g,off,true),b=render(mixed,3,g,compact);
    assert(a.next==b.next&&b.next==3&&a.marks==b.marks&&b.marks.empty());
  }
  // A large punctuation run must still make bounded, linear progress; the
  // width callback is also exercised by hanging punctuation and hyphenation.
  std::string longRun;for(unsigned i=0;i<4096;++i)longRun+="。”";
  size_t measured=0,from=0,pages=0;
  auto counted=[&](uint32_t cp){++measured;return metrics(cp);};
  while(from<longRun.size()){
    const size_t next=typography_experiment::page(longRun.data(),longRun.size(),from,
      geometry(),compact,counted,[](const char*,size_t,int,int){},[](){});
    assert(next>from&&next<=longRun.size());from=next;assert(++pages<100);
  }
  assert(measured<100*8192);
  // Original prose for screenshots; no copyrighted book text or system fonts.
  const std::string prose=
    "雨停以后，林舟推开书店的门。她问：“《春日》到了吗？”店员指着窗边说：“就在那边，（第二层）最左边。”\n\n"
    "桌上的笔记写着：“‘留白’也属于文字。”她读到这里，停了一会儿。句号、引号和括号挤在一起，页面却不该因此出现一个空洞。\n\n"
    "她翻到第12章，在EPUB3文件里读到Unicode与UTF-8的区别。书中还介绍LaTeX，并用2026年的一页实验记录说明布局变化。\n\n"
    "“真的吗？！”朋友问。她笑着回答：“试试看吧。”省略号……和破折号——仍要保留自己的宽度；改变的是空白，字形保持原来的样子。\n\n"
    "离开时，雨已经停了。她把笔记收进包里，又回头看了一眼那扇亮着灯的窗。";
  auto g=geometry();
  writeImage("typography-current.pgm",render(prose,0,g,off,true));
  writeImage("typography-punctuation.pgm",render(prose,0,g,compact));
  writeImage("typography-autospace.pgm",render(prose,0,g,gap));
  auto advanced=both;advanced.latinPixels=28;advanced.balanceLines=true;advanced.strictBreaks=true;advanced.opticalMargin=4;
  const std::string english="“A careful representation of extraordinary circumstances” is a phrase that asks a narrow page to balance its words. Typography should keep the spaces calm, with AV, To and Wa pairs naturally positioned.\n\n"
    "Each paragraph has a rhythm. When a long word reaches the edge, a bounded comparison with the following line may avoid an unnecessary hyphen. The original text and reading position remain unchanged.\n\n"
    "A short final paragraph gives the page room to breathe.";
  writeImage("typography-english-balanced.pgm",render(english,0,g,advanced));
  puts("Typography experiments: disabled baseline identical, bounded blank-only trim, explicit-space barriers, deterministic source pagination, native-font previews: PASS");
}
