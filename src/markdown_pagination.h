#pragma once
#include "markdown_document.h"
#include "pagination.h"
#include "reader_typography.h"
#include <algorithm>
#include <cstdint>

namespace markdown_page {
struct Box { int width=0,ascent=0,descent=0,inkLeft=0,inkRight=0;bool ink=false;unsigned nativePixels=0; };
struct Geometry { int left=24,right=510,top=24,bottom=916,lineHeight=40; bool hyphenate=true,justify=true,advanced=false; };
inline bool formula(const markdown::Run& run){
  return run.kind==markdown::Kind::MathInline||run.kind==markdown::Kind::MathBlock;
}
template<class Document>
inline const markdown::Run& runAt(const Document& doc,size_t at){
  size_t first=0,last=doc.runCount();
  while(first<last){size_t mid=first+(last-first)/2;
    if(doc.run(mid).end<=at)first=mid+1;else last=mid;
  }
  return doc.run(std::min(first,doc.runCount()-1));
}
// Right-biased source mapping skips generated indentation whose source span
// is empty. Recover only same-anchor code spaces on this line; replaying the
// preceding generated newlines could return the same page anchor forever.
template<class Document,class Tick>
size_t pageStart(const Document& doc,size_t from,Tick tick){
  if(from>=doc.sourceSize())return doc.size();
  size_t at=doc.renderOffset(from),examined=0;
  if(at<doc.size()){const auto& run=runAt(doc,at);if(formula(run))at=run.begin;}
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
template<class Document,class Renderer,class Tick>
size_t page(const Document& doc,size_t from,bool draw,
            Geometry geometry,Renderer& renderer,Tick tick,reader_typography::Style style={},int* usedBottom=nullptr){
  if(usedBottom)*usedBottom=geometry.top;
  if(!doc.size()||!doc.runCount())return doc.sourceSize();
  if(geometry.right<=geometry.left||geometry.bottom<=geometry.top)return from;
  const char* text=doc.text();size_t p=pageStart(doc,from,tick);
  int y=geometry.top;unsigned hyphenLines=0,qualityAttempts=0;
  const int unit=geometry.advanced?64:1;
  auto pixel=[&](int value){return geometry.advanced?(value+32)/64:value;};
  const uintptr_t begin=reinterpret_cast<uintptr_t>(text),end=begin+doc.size();
  // A source-position lookup is stable across speculative/repeated width calls.
  // Code, math and generated rules are barriers on both sides. Never insert
  // presentation bytes into the Markdown source-offset bridge.
  auto leading=[&](size_t at,size_t lineBegin,const Box& box){
    if(at>=doc.size()||at<lineBegin)return 0;
    const auto& currentRun=runAt(doc,at);
    auto prose=[](const markdown::Run& run){return run.kind==markdown::Kind::Text&&!(run.flags&(markdown::Code|markdown::Preformatted));};
    if(!prose(currentRun))return 0;
    const uint32_t current=pagination::codepoint(text+at,pagination::utf8Bytes(text+at,doc.size()-at));
    if(at==lineBegin){
      if(geometry.advanced&&pagination::opening(current)&&box.nativePixels&&box.ink)
        return -std::min(4,std::max(0,box.width-box.inkLeft)/3)*unit;
      return 0;
    }
    size_t before=at-1;while(before&&(uint8_t(text[before])&0xc0)==0x80)--before;
    const auto& previousRun=runAt(doc,before);
    if(!prose(previousRun))return 0;
    const uint32_t previous=pagination::codepoint(text+before,at-before);
    int result=0;
    if(style.punctuation&&typography_experiment::compressiblePair(previous,current)){
      const auto previousBox=renderer.glyph(previousRun,text+before,at-before);
      reader_typography::Metrics a,b;
      a.advance=previousBox.width;a.ink=previousBox.ink;a.inkLeft=previousBox.inkLeft;a.inkRight=previousBox.inkRight;
      b.advance=box.width;b.ink=box.ink;b.inkLeft=box.inkLeft;b.inkRight=box.inkRight;
      result=reader_typography::adjustment(previous,current,style,a,b)*unit;
    }else if(style.flags())result=reader_typography::adjustment(previous,current,style,{},{} )*unit;
    if(geometry.advanced&&(box.nativePixels==24||box.nativePixels==28)&&
        currentRun.flags==previousRun.flags&&currentRun.language==previousRun.language)
      result+=latin_metrics::kern64(box.nativePixels,previous,current);
    return result;
  };
  auto glyphWidth=[&](const char* ch,size_t n,size_t at,size_t lineBegin,const Box& box){
    int value=box.width*unit;
    if(geometry.advanced&&(box.nativePixels==24||box.nativePixels==28))
      value=latin_metrics::advance64(box.nativePixels,pagination::codepoint(ch,n),box.width);
    if(at<doc.size())value+=leading(at,lineBegin,box);
    return std::max(0,value);
  };
  auto makeWidth=[&](size_t lineBegin){return [&,lineBegin](const char* ch,size_t n){
    const uintptr_t address=reinterpret_cast<uintptr_t>(ch);
    const bool source=address>=begin&&address<end;
    const size_t at=source?size_t(address-begin):doc.size();
    const auto& run=runAt(doc,source?at:lineBegin);
    if(source&&formula(run))return renderer.math(run).width*unit;
    if(source&&run.kind==markdown::Kind::Rule)return (geometry.right-geometry.left)*unit;
    return glyphWidth(ch,n,at,lineBegin,renderer.glyph(run,ch,n));
  };};
  size_t atomRun=std::min(doc.runAt(p),doc.runCount()-1);
  auto atomEnd=[&](size_t at){if(at<doc.run(atomRun).begin||at>=doc.run(atomRun).end)atomRun=std::min(doc.runAt(at),doc.runCount()-1);const auto& run=doc.run(atomRun);return formula(run)?size_t(run.end):at;};
  auto line=[&](size_t from,pagination::Geometry g,const decltype(makeWidth(0))& width,bool allowHyphen){
    const auto& firstRun=runAt(doc,from);
    if(firstRun.kind==markdown::Kind::MathBlock){pagination::Line row;row.begin=row.end=row.next=from;if(firstRun.end>doc.size())return row;row.end=row.next=firstRun.end;row.advance=renderer.math(firstRun).width*unit;row.paragraphEnd=true;row.compatible=false;return row;}
    auto row=pagination::line(text,doc.size(),from,g,width,tick,allowHyphen,atomEnd);
    // A display atom terminates preceding prose even when canonical extraction
    // did not insert a newline before the MathML element.
    for(size_t r=doc.runAt(from);r<doc.runCount()&&doc.run(r).begin<row.end;++r)if(doc.run(r).kind==markdown::Kind::MathBlock){
      return pagination::line(text,size_t(doc.run(r).begin),from,g,width,tick,allowHyphen,atomEnd);
    }return row;
  };
  while(p<doc.size()){
    const auto& first=runAt(doc,p);const auto width=makeWidth(p);
    pagination::Geometry g;g.left=geometry.left+std::min<int>(first.indent,8)*18;
    g.right=geometry.right;g.hangLimit=std::min(12,540-g.right);
    const bool prose=(first.flags&(markdown::Code|markdown::Preformatted))==0;
    g.language=first.language;
    g.hyphenate=geometry.hyphenate&&prose&&g.language!=text_language::Language::None;
    g.justify=geometry.justify&&prose&&(first.flags&markdown::Heading)==0;
    const bool display=first.kind==markdown::Kind::MathBlock;
    if(display||first.kind==markdown::Kind::Rule||
        (formula(first)&&renderer.math(first).width>g.right-g.left))g.left=geometry.left;
    g.strictBreaks=geometry.advanced;
    g.left*=unit;g.right*=unit;g.hangLimit*=unit;
    const bool lastLine=y+geometry.lineHeight*2>geometry.bottom;
    auto row=line(p,g,width,hyphenLines<2&&!lastLine);
    if(row.hyphen&&row.end>row.begin&&
        (runAt(doc,row.end-1).flags&(markdown::Code|markdown::Preformatted))){
      g.hyphenate=false;
      row=line(p,g,width,false);
    }
    // A line crossing an explicit language boundary must never apply the first
    // span's dictionary to a different language later in that line.
    bool mixedLanguage=false;
    for(size_t r=doc.runAt(p);r<doc.runCount()&&doc.run(r).begin<row.end;++r)
      if(doc.run(r).language!=first.language)mixedLanguage=true;
    if(row.hyphen&&mixedLanguage){g.hyphenate=false;row=line(p,g,width,false);}
    if(row.next<=p)return from;
    if(geometry.advanced&&prose&&!display&&!(first.flags&markdown::Heading)&&!mixedLanguage&&
        !lastLine&&qualityAttempts<2){
      // Speculative lines must not borrow a dictionary or indentation from a
      // different block, language, code span or mathematical object.
      bool safe=true;
      const size_t limit=std::min(doc.size(),row.next+line_quality::MAX_SCAN_BYTES);
      for(size_t r=doc.runAt(p);r<doc.runCount()&&doc.run(r).begin<limit;++r){
        const auto& run=doc.run(r);
        if(run.kind!=markdown::Kind::Text||run.language!=first.language||run.indent!=first.indent||
            (run.flags&(markdown::Code|markdown::Preformatted|markdown::Heading))){safe=false;break;}
      }
      if(safe){
        line_quality::Budget quality;
        row=line_quality::choose(text,doc.size(),p,g,row,makeWidth,tick,hyphenLines,
          y+geometry.lineHeight*3>geometry.bottom,&quality);
        if(quality.measured)++qualityAttempts;
      }
    }
    if(y>geometry.top&&prose&&row.end>row.begin){
      const bool start=p==0||text[p-1]=='\n';
      if((first.flags&markdown::Heading)&&start&&
          y+3*geometry.lineHeight>geometry.bottom&&3*geometry.lineHeight<=geometry.bottom-geometry.top)break;
      // Keep a paragraph's opening pair together; avoid leaving a single final
      // line on the next page. These bounded decisions are shared by painting.
      // Moving this line must not strand the previous discretionary hyphen
      // across a page boundary. Word continuity takes priority over widows.
      if(!(first.flags&markdown::Heading)&&!row.paragraphEnd&&lastLine&&hyphenLines==0){
        if(start)break;
        const auto following=line(row.next,g,makeWidth(row.next),false);
        if(following.paragraphEnd&&following.end>following.begin)break;
      }
    }
    if(lastLine&&row.emergency&&y>geometry.top)break;
    int ascent=31,descent=8,measuredAdvance=0;bool codeLine=false;
    for(size_t at=row.begin;at<row.end;){
      const auto& run=runAt(doc,at);
      size_t n=formula(run)?run.end-at:pagination::utf8Bytes(text+at,row.end-at);
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
      const uint32_t cp=formula(run)?0xfffcu:pagination::codepoint(text+at,n);
      if(!pagination::invisible(cp)){
        if(formula(run))measuredAdvance+=box.width*unit;
        else if(run.kind==markdown::Kind::Rule)measuredAdvance+=(geometry.right-geometry.left)*unit;
        else if(cp=='\t'||pagination::noBreakSpace(cp))measuredAdvance+=width(" ",1);
        else measuredAdvance+=glyphWidth(text+at,n,at,p,box);
      }
      at+=n;
    }
    int height=std::max(geometry.lineHeight,ascent+descent+1);
    if(row.end==row.begin)height=geometry.lineHeight/2;
    if(display)height+=16;
    if(y+height>geometry.bottom&&y>geometry.top)break;
    // Math admission guarantees that a standalone object fits a fresh page.
    if(y+height>geometry.bottom)return from;
    if(draw){
      const int baseline=y+ascent+(display?8:0);
      int x=g.left;if(display)x+=(g.right-g.left-row.advance)/2;
      const int spare=g.right-g.left-row.advance;
      const int spaceWidth=std::max(0,width(" ",1));
      const bool justify=g.justify&&!codeLine&&!display&&!row.paragraphEnd&&row.latinText&&row.compatible&&
        !row.formatted&&!row.emergency&&row.spaces>0&&spare>=0&&
        spare<=row.spaces*std::max(1,spaceWidth*3/4);
      unsigned usedSpaces=0;
      if(first.flags&markdown::Quote)renderer.rule(pixel(g.left)-8,y,1,height-2);
      for(size_t at=row.begin;at<row.end;){
        const auto& run=runAt(doc,at);const size_t n=formula(run)?run.end-at:pagination::utf8Bytes(text+at,row.end-at);
        const uint32_t cp=formula(run)?0xfffcu:pagination::codepoint(text+at,n);
        if(formula(run)){
          renderer.drawMath(run,pixel(x),baseline);x+=renderer.math(run).width*unit;
        }else if(run.kind==markdown::Kind::Rule){
          renderer.rule(pixel(g.left),y+height/2,pixel(g.right-g.left),1);x=g.right;
        }else if(!pagination::invisible(cp)){
          const auto box=renderer.glyph(run,text+at,n);
          renderer.drawGlyph(run,text+at,n,pixel(x+leading(at,p,box)),baseline);
          x+=(cp=='\t'||pagination::noBreakSpace(cp))?width(" ",1):glyphWidth(text+at,n,at,p,box);
          if(justify&&pagination::space(cp)&&at>row.begin){
            const int before=spare*usedSpaces/row.spaces;++usedSpaces;
            x+=spare*usedSpaces/row.spaces-before;
          }
        }
        at+=n;
      }
      if(row.hyphen)renderer.drawGlyph(runAt(doc,row.end-1),"-",1,pixel(x),baseline);
    }
    hyphenLines=row.hyphen?hyphenLines+1:0;p=row.next;y+=height;if(usedBottom)*usedBottom=y;tick();
  }
  return p==doc.size()?doc.sourceSize():doc.sourceOffset(p);
}
}
