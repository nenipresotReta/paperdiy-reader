#pragma once
#include "epub_document.h"
#include "content_source.h"
#include "pagination.h"
#include "vertical_font.h"
#include <algorithm>
namespace epub_vertical {
struct Geometry {int left=24,right=510,top=24,bottom=916,pixels=28,columnPitch=40;};
struct Atom {uint32_t begin=0,end=0,cp=0;uint16_t extent=0;uint8_t digits=0;const epub_document::Ruby* ruby=nullptr;};
inline size_t count(const char* text,size_t begin,size_t end){size_t n=0;while(begin<end){begin+=pagination::utf8Bytes(text+begin,end-begin);++n;}return n;}
// Semantic ruby remains portable across font presets. Admit its reading only
// when every nonblank character has a native glyph at this exact small size;
// otherwise the composer emits the complete canonical group as ordinary text.
inline unsigned rubyAdvance(uint32_t cp,unsigned pixels){const auto* glyph=reader_font::findGlyph(vertical_font::font(pixels),cp);return glyph?glyph->advance:pixels;}
inline size_t rubyExtent(const char* text,size_t begin,size_t end,unsigned pixels){size_t extent=0;while(begin<end){const size_t bytes=pagination::utf8Bytes(text+begin,end-begin);extent+=rubyAdvance(pagination::codepoint(text+begin,bytes),pixels);begin+=bytes;}return extent;}
inline bool rubyAvailable(const epub_document::Document& doc,const epub_document::Ruby& ruby,unsigned pixels){
  const char* text=doc.text();for(size_t at=ruby.noteBegin;at<ruby.noteEnd;){const size_t bytes=pagination::utf8Bytes(text+at,ruby.noteEnd-at);const uint32_t cp=pagination::codepoint(text+at,bytes);
    if(reader_font::blankAdvance(cp,int(pixels))<0&&cp!=' '&&cp!=0x3000&&!vertical_font::has(cp,pixels))return false;at+=bytes;
  }return true;
}
template<class Renderer>
unsigned bodyAdvance(uint32_t cp,int pixels,Renderer& renderer){return unsigned(cp==' '?pixels/2:vertical_font::sideways(cp,unsigned(pixels))?std::min(pixels+vertical_font::SIDEWAYS_EXTRA,std::max(1,renderer.advance(cp))):pixels);}
template<class Renderer>
size_t baseExtent(const char* text,size_t begin,size_t end,int pixels,Renderer& renderer){size_t extent=0;while(begin<end){const size_t bytes=pagination::utf8Bytes(text+begin,end-begin);extent+=std::max(unsigned(pixels),bodyAdvance(pagination::codepoint(text+begin,bytes),pixels,renderer));begin+=bytes;}return extent;}
template<class Renderer>
Atom atom(const epub_document::Document& doc,size_t at,size_t end,int pixels,Renderer& renderer){
  Atom a;a.begin=uint32_t(at);const char* text=doc.text();
  const auto* ruby=doc.rubyAt(at);
  if(ruby&&rubyAvailable(doc,*ruby,unsigned(pixels/2))){a.ruby=ruby;a.end=a.ruby->end;a.cp=pagination::codepoint(text+a.ruby->begin,pagination::utf8Bytes(text+a.ruby->begin,a.ruby->baseEnd-a.ruby->begin));
    a.extent=uint16_t(std::max(baseExtent(text,a.ruby->begin,a.ruby->baseEnd,pixels,renderer),rubyExtent(text,a.ruby->noteBegin,a.ruby->noteEnd,unsigned(pixels/2))));return a;}
  size_t bytes=pagination::utf8Bytes(text+at,end-at);a.cp=pagination::codepoint(text+at,bytes);a.end=uint32_t(at+bytes);
  if(doc.combineAt(at)&&a.cp>='0'&&a.cp<='9'){
    const auto& run=doc.run(doc.runAt(at));size_t start=at,limit=std::min<size_t>(run.end,end);while(start<limit&&text[start]>='0'&&text[start]<='9')++start;
    // More than two digits stay ordinary sideways text; never split into pairs.
    if(start-at<=2&&(at==run.begin||text[at-1]<'0'||text[at-1]>'9')){a.digits=uint8_t(start-at);a.end=uint32_t(start);}
  }
  a.extent=uint16_t(a.digits?unsigned(pixels):bodyAdvance(a.cp,pixels,renderer));return a;
}
inline bool legal(const Atom& a,const Atom& b){return !pagination::opening(a.cp)&&!pagination::prohibitedStart(b.cp)&&!((a.cp==0x2014||a.cp==0x2026)&&a.cp==b.cp);}
template<class Renderer,class Tick>
size_t page(const epub_document::Document& doc,size_t from,bool draw,const Geometry& g,Renderer& renderer,Tick tick){
  if(!doc.ready()||from<reader_content::begin()||from>=doc.size()||!doc.verticalAt(from)||g.pixels<12||g.bottom-g.top<g.pixels||g.right-g.left<g.pixels+g.pixels/2+2+vertical_font::SIDEWAYS_EXTRA)return from;
  constexpr unsigned MAX_ATOMS=96;Atom atoms[MAX_ATOMS];const size_t end=std::min(doc.modeEnd(from),readerContentLimit());const char* text=doc.text();size_t at=from;
  const int rubyPixels=g.pixels/2,pitch=std::max(g.columnPitch,g.pixels+rubyPixels+2+vertical_font::SIDEWAYS_EXTRA);
  for(int x=g.right-g.pixels-rubyPixels-2;x>=g.left+vertical_font::SIDEWAYS_EXTRA&&at<end;x-=pitch){
    unsigned n=0;int extent=0;size_t next=at;bool hardBreak=false;
    while(next<end&&n<MAX_ATOMS){
      if(text[next]=='\n'||text[next]=='\r'){do{++next;}while(next<end&&(text[next]=='\n'||text[next]=='\r'));hardBreak=true;break;}
      Atom a=atom(doc,next,end,g.pixels,renderer);
      if(a.extent>g.bottom-g.top){ // unsupported over-height ruby remains readable source
        a.ruby=nullptr;const size_t bytes=pagination::utf8Bytes(text+next,end-next);a.cp=pagination::codepoint(text+next,bytes);a.end=uint32_t(next+bytes);a.extent=uint16_t(bodyAdvance(a.cp,g.pixels,renderer));
      }
      if(extent+a.extent>g.bottom-g.top)break;
      atoms[n++]=a;extent+=a.extent;next=a.end;
    }
    if(!n){if(next>at){at=next;tick();continue;}return at;}
    if(!hardBreak&&next<end){const Atom following=atom(doc,next,end,g.pixels,renderer);unsigned keep=n;
      while(keep>1&&!legal(atoms[keep-1],keep==n?following:atoms[keep]))--keep;
      if(keep<n){n=keep;next=atoms[n-1].end;}
    }
    if(draw){int y=g.top;for(unsigned i=0;i<n;++i){const auto& a=atoms[i];
      if(a.ruby){const auto& r=*a.ruby;size_t p=r.begin;const size_t baseTotal=baseExtent(text,p,r.baseEnd,g.pixels,renderer);size_t baseOffset=0;
        while(p<r.baseEnd){size_t bytes=pagination::utf8Bytes(text+p,r.baseEnd-p);uint32_t cp=pagination::codepoint(text+p,bytes);const unsigned advance=std::max(unsigned(g.pixels),bodyAdvance(cp,g.pixels,renderer));int pos=y+int((2*baseOffset+advance)*a.extent/(2*baseTotal))-int(advance)/2;renderer.glyph(cp,g.pixels,x,pos,vertical_font::sideways(cp,g.pixels),p,p+bytes);p+=bytes;baseOffset+=advance;}
        p=r.noteBegin;const size_t noteExtent=rubyExtent(text,p,r.noteEnd,unsigned(rubyPixels));size_t offset=0;
        while(p<r.noteEnd){size_t bytes=pagination::utf8Bytes(text+p,r.noteEnd-p);uint32_t cp=pagination::codepoint(text+p,bytes);const unsigned advance=rubyAdvance(cp,unsigned(rubyPixels));int pos=y+int((2*offset+advance)*a.extent/(2*noteExtent))-int(advance)/2;renderer.glyph(cp,rubyPixels,x+g.pixels+2,pos,false,p,p+bytes);p+=bytes;offset+=advance;}
      }else if(a.digits){for(unsigned j=0;j<a.digits;++j)renderer.glyph(uint8_t(text[a.begin+j]),rubyPixels,x+(g.pixels-a.digits*rubyPixels)/2+j*rubyPixels,y+(g.pixels-rubyPixels)/2,false,a.begin+j,a.begin+j+1);}
      else renderer.glyph(a.cp,g.pixels,x,y,vertical_font::sideways(a.cp,g.pixels),a.begin,a.end);
      y+=a.extent;
    }}at=next;tick();
  }return at;
}
} // namespace epub_vertical
