#pragma once
#include "markdown_document.h"
#include "pagination.h"
#include "reader_typography.h"
#include <algorithm>
#include <cstdint>

namespace markdown_page {
struct Box { int width=0,ascent=0,descent=0,inkLeft=0,inkRight=0;bool ink=false; };
struct Geometry { int left=24,right=510,top=24,bottom=916,lineHeight=40; };
inline bool formula(const markdown::Run& run){
  return run.kind==markdown::Kind::MathInline||run.kind==markdown::Kind::MathBlock;
}
inline const markdown::Run& runAt(const markdown::Document& doc,size_t at){
  size_t first=0,last=doc.runCount();
  while(first<last){size_t mid=first+(last-first)/2;
    if(doc.run(mid).end<=at)first=mid+1;else last=mid;
  }
  return doc.run(std::min(first,doc.runCount()-1));
}
// Right-biased source mapping skips generated indentation whose source span
// is empty. Recover only same-anchor code spaces on this line; replaying the
// preceding generated newlines could return the same page anchor forever.
template<class Tick>
size_t pageStart(const markdown::Document& doc,size_t from,Tick tick){
  if(from>=doc.sourceSize())return doc.size();
  size_t at=doc.renderOffset(from),examined=0;
  while(at){
    const char ch=doc.text()[at-1];
    if(ch!=' '&&ch!='\t')break;
    const auto& run=runAt(doc,at-1);
    if((run.flags&(markdown::Code|markdown::Preformatted))!=(markdown::Code|markdown::Preformatted)||
       doc.sourceOffset(at-1)!=from)break;
    --at;if((++examined&255u)==0)tick();
  }
  return at;
}
// Reuse the proven English/CJK line breaker, while line height and baselines
// come from every object actually placed on that line. Source offsets, never
// presentation bytes or page numbers, cross the persistence boundary.
template<class Renderer,class Tick>
size_t page(const markdown::Document& doc,size_t from,bool draw,
            Geometry geometry,Renderer& renderer,Tick tick,reader_typography::Style style={}){
  if(!doc.size()||!doc.runCount())return doc.sourceSize();
  if(geometry.right<=geometry.left||geometry.bottom<=geometry.top)return from;
  const char* text=doc.text();size_t p=pageStart(doc,from,tick);
  int y=geometry.top;unsigned hyphenLines=0;
  const uintptr_t begin=reinterpret_cast<uintptr_t>(text),end=begin+doc.size();
  // A source-position lookup is stable across speculative/repeated width calls.
  // Code, math and generated rules are barriers on both sides. Never insert
  // presentation bytes into the Markdown source-offset bridge.
  auto leading=[&](size_t at){
    if(!style.flags()||at<=p||at>=doc.size())return 0;
    size_t before=at-1;while(before&&(uint8_t(text[before])&0xc0)==0x80)--before;
    const auto& currentRun=runAt(doc,at);const auto& previousRun=runAt(doc,before);
    auto prose=[](const markdown::Run& run){return run.kind==markdown::Kind::Text&&!(run.flags&(markdown::Code|markdown::Preformatted));};
    if(!prose(currentRun)||!prose(previousRun))return 0;
    const uint32_t current=pagination::codepoint(text+at,pagination::utf8Bytes(text+at,doc.size()-at));
    const uint32_t previous=pagination::codepoint(text+before,at-before);
    auto metrics=[&](size_t offset){
      const auto box=renderer.glyph(runAt(doc,offset),text+offset,pagination::utf8Bytes(text+offset,doc.size()-offset));
      reader_typography::Metrics result;result.advance=box.width;
      result.ink=box.ink;result.inkLeft=box.inkLeft;result.inkRight=box.inkRight;return result;
    };
    if(style.punctuation&&typography_experiment::compressiblePair(previous,current))
      return reader_typography::adjustment(previous,current,style,metrics(before),metrics(at));
    return reader_typography::adjustment(previous,current,style,{},{});
  };
  auto width=[&](const char* ch,size_t n){
    const uintptr_t address=reinterpret_cast<uintptr_t>(ch);
    const auto& run=runAt(doc,address>=begin&&address<end?size_t(address-begin):p);
    if(address>=begin&&address<end&&formula(run))return renderer.math(run).width;
    if(address>=begin&&address<end&&run.kind==markdown::Kind::Rule)return geometry.right-geometry.left;
    const int adjustment=address>=begin&&address<end?leading(size_t(address-begin)):0;
    return std::max(0,renderer.glyph(run,ch,n).width+adjustment);
  };
  while(p<doc.size()){
    const auto& first=runAt(doc,p);
    pagination::Geometry g;g.left=geometry.left+std::min<int>(first.indent,8)*18;
    g.right=geometry.right;g.hangLimit=std::min(12,540-g.right);
    g.hyphenate=(first.flags&(markdown::Code|markdown::Preformatted))==0;
    g.justify=g.hyphenate&&(first.flags&markdown::Heading)==0;
    const bool display=first.kind==markdown::Kind::MathBlock;
    if(display||first.kind==markdown::Kind::Rule||
        (formula(first)&&renderer.math(first).width>g.right-g.left))g.left=geometry.left;
    const bool lastLine=y+geometry.lineHeight*2>geometry.bottom;
    auto row=pagination::line(text,doc.size(),p,g,width,tick,hyphenLines<2&&!lastLine);
    if(row.hyphen&&row.end>row.begin&&
        (runAt(doc,row.end-1).flags&(markdown::Code|markdown::Preformatted))){
      g.hyphenate=false;
      row=pagination::line(text,doc.size(),p,g,width,tick,false);
    }
    if(row.next<=p)return from;
    if(lastLine&&row.emergency&&y>geometry.top)break;
    int ascent=31,descent=8,measuredAdvance=0;bool codeLine=false;
    for(size_t at=row.begin;at<row.end;){
      const auto& run=runAt(doc,at);
      size_t n=pagination::utf8Bytes(text+at,row.end-at);
      const auto box=formula(run)?renderer.math(run):renderer.glyph(run,text+at,n);
      const int nextAscent=std::max(ascent,box.ascent),nextDescent=std::max(descent,box.descent);
      // Two individually admissible formulas can have opposing large ascent /
      // descent. Split before the second object if their combined baseline
      // would exceed even a fresh page. Horizontal fitting alone is insufficient.
      if(at>row.begin&&nextAscent+nextDescent+1+(display?16:0)>geometry.bottom-geometry.top){
        row.end=at;row.next=at;row.advance=measuredAdvance;
        row.hyphen=false;row.paragraphEnd=false;row.emergency=false;
        row.compatible=false; // A height-constrained line is never justified.
        break;
      }
      codeLine|=(run.flags&(markdown::Code|markdown::Preformatted))!=0;
      ascent=nextAscent;descent=nextDescent;
      measuredAdvance+=pagination::advance(text+at,n,pagination::codepoint(text+at,n),width);
      at+=n;
    }
    int height=std::max(geometry.lineHeight,ascent+descent+1);
    if(row.end==row.begin)height=geometry.lineHeight/2;
    if(display)height+=16;
    if(y+height>geometry.bottom&&y>geometry.top)break;
    // Math admission guarantees that a standalone object fits a fresh page.
    if(y+height>geometry.bottom)return from;
    const int baseline=y+ascent+(display?8:0);
    int x=g.left;if(display)x+=(g.right-g.left-row.advance)/2;
    const int spare=g.right-g.left-row.advance;
    const int spaceWidth=std::max(0,width(" ",1));
    bool justify=g.justify&&!codeLine&&!display&&!row.paragraphEnd&&row.latinText&&row.compatible&&
      !row.formatted&&!row.emergency&&row.spaces>0&&spare>=0&&
      spare<=row.spaces*std::max(1,spaceWidth*3/4);
    unsigned usedSpaces=0;
    if(draw&&(first.flags&markdown::Quote))renderer.rule(g.left-8,y,1,height-2);
    for(size_t at=row.begin;at<row.end;){
      const auto& run=runAt(doc,at);const size_t n=pagination::utf8Bytes(text+at,row.end-at);
      const uint32_t cp=pagination::codepoint(text+at,n);
      if(formula(run)){
        if(draw)renderer.drawMath(run,x,baseline);x+=renderer.math(run).width;
      }else if(run.kind==markdown::Kind::Rule){
        if(draw)renderer.rule(g.left,y+height/2,g.right-g.left,1);x=g.right;
      }else if(!pagination::invisible(cp)){
        if(draw)renderer.drawGlyph(run,text+at,n,x+leading(at),baseline);
        x+=pagination::advance(text+at,n,cp,width);
        if(justify&&pagination::space(cp)&&at>row.begin){
          const int before=spare*usedSpaces/row.spaces;++usedSpaces;
          x+=spare*usedSpaces/row.spaces-before;
        }
      }
      at+=n;
    }
    if(row.hyphen&&draw)renderer.drawGlyph(runAt(doc,row.end-1),"-",1,x,baseline);
    hyphenLines=row.hyphen?hyphenLines+1:0;p=row.next;y+=height;tick();
  }
  return p==doc.size()?doc.sourceSize():doc.sourceOffset(p);
}
}
