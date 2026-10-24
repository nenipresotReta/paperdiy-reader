#pragma once
// Contextual spacing core shared by production and the original A/B laboratory.
// Shares the deployed line breaker; changes only pen positions and advances.
#include "pagination.h"
#include "latin_metrics.h"
#include "line_quality.h"

namespace typography_experiment {
struct Options {
  bool compressAdjacentPunctuation=false;
  int hanLatinGap=0; // Candidate 4 px; caller may compare 0 / 4 / 7 explicitly.
  int maxPunctuationTrim=14;
  int minInkGap=2;
  unsigned latinPixels=0; // Native 24/28 px Latin metrics; 0 preserves the laboratory baseline.
  bool balanceLines=false;
  bool strictBreaks=false;
  int opticalMargin=0;
};
struct Metrics { int advance=0,inkLeft=0,inkRight=0;bool ink=false; };
enum class Punctuation { None,Open,Close,Stop };
inline Punctuation punctuation(uint32_t cp){
  switch(cp){
    case 0x2018:case 0x201c:case 0x3008:case 0x300a:case 0x300c:case 0x300e:
    case 0x3010:case 0x3014:case 0xff08:case 0xff3b:case 0xff5b:return Punctuation::Open;
    case 0x2019:case 0x201d:case 0x3009:case 0x300b:case 0x300d:case 0x300f:
    case 0x3011:case 0x3015:case 0xff09:case 0xff3d:case 0xff5d:return Punctuation::Close;
    case 0x3001:case 0x3002:case 0xff0c:case 0xff0e:case 0xff01:case 0xff1f:
    case 0xff1a:case 0xff1b:return Punctuation::Stop;
    default:return Punctuation::None;
  }
}
inline bool compressiblePair(uint32_t before,uint32_t after){
  const auto a=punctuation(before),b=punctuation(after);
  return (a==Punctuation::Open&&b==Punctuation::Open)||
    ((a==Punctuation::Close||a==Punctuation::Stop)&&b!=Punctuation::None);
}
inline bool han(uint32_t cp){
  return (cp>=0x3400&&cp<=0x4dbf)||(cp>=0x4e00&&cp<=0x9fff)||
    (cp>=0xf900&&cp<=0xfaff)||(cp>=0x20000&&cp<=0x323af);
}
inline bool asciiWord(uint32_t cp){
  return (cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z')||(cp>='0'&&cp<='9');
}
inline int adjustment(uint32_t previous,uint32_t current,const Options& options,
                      const Metrics& left,const Metrics& right){
  if(options.compressAdjacentPunctuation&&compressiblePair(previous,current)&&left.ink&&right.ink){
    const int blank=left.advance-left.inkRight+right.inkLeft;
    const int available=std::max(0,blank-std::max(0,options.minInkGap));
    return -std::min(std::max(0,options.maxPunctuationTrim),
      std::min(available,std::max(0,right.advance-1)));
  }
  // Explicit spaces, punctuation, combining marks and variation selectors
  // are barriers. No guessed grapheme/locale shaping.
  if((han(previous)&&asciiWord(current))||(asciiWord(previous)&&han(current)))
    return std::max(0,std::min(options.hanLatinGap,14));
  return 0;
}
template<class Measure>
int leading(const char* text,size_t length,size_t at,size_t lineBegin,
            const Options& options,Measure measure){
  if(at<=lineBegin||at>=length)return 0;
  const uint32_t previous=pagination::previous(text,at);
  const uint32_t current=pagination::codepoint(text+at,pagination::utf8Bytes(text+at,length-at));
  if(options.compressAdjacentPunctuation&&compressiblePair(previous,current))
    return adjustment(previous,current,options,measure(previous),measure(current));
  return adjustment(previous,current,options,Metrics{},Metrics{});
}

template<class Measure,class Draw,class Tick>
size_t page(const char* text,size_t length,size_t from,pagination::Geometry g,
            const Options& options,Measure measure,Draw draw,Tick tick,bool paint=true){
  if(g.right<=g.left||g.lineHeight<=0||g.bottom<g.top)return from;
  size_t p=from;unsigned hyphenLines=0,qualityAttempts=0;
  const bool precise=options.latinPixels==24||options.latinPixels==28;
  const int unit=precise?64:1;
  pagination::Geometry horizontal=g;
  horizontal.left*=unit;horizontal.right*=unit;horizontal.hangLimit*=unit;
  horizontal.strictBreaks=options.strictBreaks;
  const uintptr_t begin=reinterpret_cast<uintptr_t>(text),end=begin+length;
  const bool continuation=from&&from<length&&
    (pagination::latin(pagination::previous(text,from))||pagination::previous(text,from)==0xad)&&
    pagination::latin(pagination::codepoint(text+from,pagination::utf8Bytes(text+from,length-from)));
  const bool multiLine=g.top+g.lineHeight<=g.bottom;
  auto offset=[&](size_t at,size_t lineBegin){
    int result=leading(text,length,at,lineBegin,options,measure)*unit;
    if(at==lineBegin&&at<length&&options.opticalMargin>0){
      const uint32_t cp=pagination::codepoint(text+at,pagination::utf8Bytes(text+at,length-at));
      if(pagination::opening(cp)){
        const auto ink=measure(cp);
        result-=std::min(std::max(0,std::min(options.opticalMargin,g.left)),
          ink.ink?std::max(0,ink.advance-ink.inkLeft)/3:0)*unit;
      }
    }
    if(precise&&at>lineBegin&&at<length){
      const uint32_t current=pagination::codepoint(text+at,pagination::utf8Bytes(text+at,length-at));
      result+=latin_metrics::kern64(options.latinPixels,pagination::previous(text,at),current);
    }
    return result;
  };
  auto makeWidth=[&](size_t lineBegin){return [&,lineBegin](const char* ch,size_t n){
    const uint32_t cp=pagination::codepoint(ch,n);
    const int fallback=measure(cp).advance;
    int value=precise?latin_metrics::advance64(options.latinPixels,cp,fallback):fallback;
    const uintptr_t address=reinterpret_cast<uintptr_t>(ch);
    // Literal discretionary hyphens have no source context or pair adjustment.
    if(address>=begin&&address<end)value+=offset(size_t(address-begin),lineBegin);
    return std::max(0,value);
  };};
  auto pixel=[&](int value){return precise?(value+32)/64:value;};
  for(int y=g.top;y<=g.bottom&&p<length;y+=g.lineHeight){
    const size_t lineBegin=p;const auto width=makeWidth(lineBegin);
    const bool lastLine=multiLine&&y+g.lineHeight>g.bottom;
    const bool allowHyphen=hyphenLines<2&&!(y==g.top&&continuation)&&!lastLine;
    auto row=pagination::line(text,length,p,horizontal,width,tick,allowHyphen);
    if(row.next<=p)return p;
    if(lastLine&&row.emergency)return p;
    if(options.balanceLines&&!lastLine&&qualityAttempts<2){
      line_quality::Budget quality;
      row=line_quality::choose(text,length,p,horizontal,row,makeWidth,tick,hyphenLines,
        y+2*g.lineHeight>g.bottom,&quality);
      if(quality.measured)++qualityAttempts;
    }
    // Indexing shares all line decisions, but does not walk glyphs a second time.
    if(paint){
      const int spare=horizontal.right-horizontal.left-row.advance;
      const int spaceWidth=std::max(0,width(" ",1));
      const bool justify=g.justify&&!row.paragraphEnd&&row.latinText&&row.compatible&&
        !row.formatted&&!row.emergency&&row.spaces>0&&spare>=0&&
        spare<=row.spaces*std::max(1,spaceWidth*3/4);
      int x=horizontal.left,usedSpaces=0;
      for(size_t at=row.begin;at<row.end;){
        const size_t n=pagination::utf8Bytes(text+at,row.end-at);
        const uint32_t cp=pagination::codepoint(text+at,n);
        if(!pagination::invisible(cp)){
          const int w=pagination::advance(text+at,n,cp,width);
          if(pagination::space(cp)||pagination::noBreakSpace(cp))draw(" ",1,pixel(x),y);
          else draw(text+at,n,pixel(x+offset(at,lineBegin)),y);
          x+=w;
          if(justify&&pagination::space(cp)&&at>row.begin){
            const int before=spare*usedSpaces/row.spaces;++usedSpaces;
            x+=spare*usedSpaces/row.spaces-before;
          }
        }
        at+=n;
      }
      if(row.hyphen)draw("-",1,pixel(x),y);
    }
    hyphenLines=row.hyphen?hyphenLines+1:0;p=row.next;tick();
  }
  return p;
}
} // namespace typography_experiment
