#pragma once
#include "console.h"
#include "pagination.h"
#include "reader.h"
#include "reading_font.h"
inline void deviceTypographyChecks() {
  prepareReaderFont();
  auto width=[](const char* p,size_t n){return bodyGlyphAdvance(pagination::codepoint(p,n));};
  unsigned passed=0;
  auto check=[&](const char* name,bool ok){
    ok=ok&&uxTaskGetStackHighWaterMark(nullptr)>=4096;
    if(ok)++passed;
    Console.printf("DEVICE_TYPESET_TEST name=%s passed=%d stack_free=%u\n",name,ok,unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  };
  pagination::Geometry g;g.left=0;g.top=0;g.bottom=0;
  const char* word="extraordinarily";int wordWidth=0;
  for(const char* p=word;*p;++p)wordWidth+=width(p,1);
  g.right=wordWidth+4;g.hyphenate=false;
  const char* text="one extraordinarily";
  auto row=pagination::line(text,strlen(text),0,g,width,[](){});
  check("whole_word",row.end==3&&row.next==4&&!row.hyphen);
  word="representation";wordWidth=0;for(const char* p=word;*p;++p)wordWidth+=width(p,1);
  g.right=wordWidth-1;g.hyphenate=true;
  row=pagination::line(word,strlen(word),0,g,width,[](){});
  check("pattern_hyphen",row.hyphen&&row.end<strlen(word)&&row.advance<=g.right);
  g.right=240;g.bottom=120;g.lineHeight=40;
  text="A careful representation of extraordinary navigation preserves each word and uses bounded spaces. Final short line.";
  bool inside=true;unsigned drawn=0;
  size_t end=pagination::page(text,strlen(text),0,g,width,
    [&](const char* p,size_t n,int x,int y){
      ++drawn;
      uint32_t cp=pagination::codepoint(p,n);
      bool hanging=pagination::closing(cp)&&x<=g.right&&x+width(p,n)>g.right;
      inside&=x>=0&&(x+width(p,n)<=g.right||hanging)&&y<=g.bottom;
    },[](){});
  size_t indexed=pagination::page(text,strlen(text),0,g,width,[](const char*,size_t,int,int){},[](){});
  check("draw_index_bounds",inside&&drawn>0&&end==indexed&&end>0);
  Console.printf("DEVICE_TYPESET_DONE passed=%u total=3 english=%d\n",passed,bookEnglishHyphens);
}
