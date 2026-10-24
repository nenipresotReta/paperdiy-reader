#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "pagination.h"
#include "pagination_checkpoint.h"

// The visible window and the canonical prefix are deliberately separate. A
// completed prefix never replaces the page being read: only explicit navigation
// may adopt it. All positions remain offsets in the original UTF-8 source.
namespace progressive_index {
constexpr size_t MAX_PAGES=65536,WINDOW_PAGES=32;
constexpr size_t BACKTRACK_BYTES=4096,BACKTRACK_PAGES=16,WORD_LOOKBEHIND=256;

inline bool continuation(char c){return (uint8_t(c)&0xc0u)==0x80u;}
inline bool word(uint32_t cp){return pagination::latin(cp)||pagination::combining(cp)||cp==0xad||cp=='\''||cp==0x2019;}
// Existing anchors can come from a different layout. Do not begin on a UTF-8
// continuation byte or in an ordinary English word; bound malformed long words.
inline uint32_t safeStart(const char* text,size_t size,uint32_t anchor){
  if(!text||anchor>=size)return anchor;
  size_t at=anchor;
  for(unsigned n=0;at&&continuation(text[at])&&n<3;++n)--at;
  const size_t limit=at>WORD_LOOKBEHIND?at-WORD_LOOKBEHIND:0;
  auto letter=[&](size_t p){return word(pagination::codepoint(text+p,pagination::utf8Bytes(text+p,size-p)));};
  while(at>limit&&letter(at)){
    size_t before=at-1;
    for(unsigned n=0;before&&continuation(text[before])&&n<3;++n)--before;
    if(before<limit||!letter(before))break;
    at=before;
  }
  return uint32_t(at);
}

class Session {
 public:
  void reset(){
    std::vector<uint32_t>().swap(canonical_);
    length_=cursor_=windowEnd_=0;active_=complete_=failed_=provisional_=false;
  }
  bool active()const{return active_;}
  bool pending()const{return active_&&!complete_&&!failed_;}
  bool complete()const{return active_&&complete_;}
  bool failed()const{return failed_;}
  bool provisional()const{return active_&&provisional_;}
  const std::vector<uint32_t>& canonical()const{return canonical_;}
  uint32_t cursor()const{return uint32_t(cursor_);}
  // Only a validated partial product may seed the canonical prefix. Recovery
  // never mutates the local display window, selected page or its exact anchor.
  bool restore(const uint32_t* pages,size_t count,uint32_t cursor){
    if(!pending()||cursor<=cursor_||
       count<canonical_.size()||!pagination_checkpoint::validPrefix(pages,count,cursor,length_))return false;
    for(size_t i=0;i<canonical_.size();++i)if(canonical_[i]!=pages[i])return false;
    canonical_.assign(pages,pages+count);cursor_=cursor;return true;
  }

  template<class Next>
  bool begin(size_t length,uint32_t start,std::vector<uint32_t>& visible,
             uint32_t& selected,Next next){
    reset();if(!length||length>UINT32_MAX||start>=length)return false;
    const size_t end=next(start);
    if(end<=start||end>length)return false;
    length_=length;windowEnd_=end;active_=provisional_=true;
    visible.assign(1,start);selected=0;
    // Opening a new book already measured the first canonical page. Reuse it.
    if(!start){canonical_.push_back(0);cursor_=end;complete_=end==length;}
    return true;
  }

  // Soft elapsed-time cap: a layout page is the smallest indivisible unit.
  // Next keeps the existing cooperative watchdog budget inside that page.
  template<class Next,class Clock>
  bool advance(size_t maxPages,uint32_t maxMs,Next next,Clock clock){
    if(!pending()||!maxPages||!maxMs)return false;
    const uint32_t started=clock();bool progressed=false;
    for(size_t count=0;count<maxPages&&pending();++count){
      if(count&&uint32_t(clock()-started)>=maxMs)break;
      if(canonical_.size()==MAX_PAGES){failed_=true;return true;}
      const size_t end=next(cursor_);
      if(end<=cursor_||end>length_){failed_=true;return true;}
      canonical_.push_back(uint32_t(cursor_));cursor_=end;progressed=true;
      if(cursor_==length_)complete_=true;
    }
    return progressed;
  }

  size_t pageEnd(const std::vector<uint32_t>& visible,uint32_t selected)const{
    if(selected>=visible.size())return UINT32_MAX;
    return selected+1<visible.size()?visible[selected+1]:provisional()?windowEnd_:length_;
  }

  template<class Next>
  bool turn(int direction,const char* text,std::vector<uint32_t>& visible,
            uint32_t& selected,Next next){
    if(!provisional()||!direction||selected>=visible.size())return false;
    const size_t start=visible[selected],end=pageEnd(visible,selected);
    if((direction<0&&!start)||(direction>0&&end>=length_))return false;
    if(complete_){
      // The containing page can overlap already-read text after reflow, but
      // must include the next unread byte (or the byte immediately before us).
      const uint32_t target=direction<0?uint32_t(start-1):uint32_t(end);
      auto at=std::upper_bound(canonical_.begin(),canonical_.end(),target);
      const uint32_t page=uint32_t(at-canonical_.begin()-1);
      visible.swap(canonical_);std::vector<uint32_t>().swap(canonical_);
      selected=page;provisional_=false;return true;
    }
    if(direction>0){
      if(selected+1<visible.size()){++selected;return true;}
      const size_t following=next(end);
      if(following<=end||following>length_)return false;
      if(visible.size()==WINDOW_PAGES)visible.erase(visible.begin());
      visible.push_back(uint32_t(end));selected=uint32_t(visible.size()-1);
      windowEnd_=following;return true;
    }
    if(selected){--selected;return true;}

    // A deep anchor has no canonical predecessor yet. Reconstruct only a
    // bounded local window. At its left edge a previous turn may repeat a little
    // text; it never skips the byte before the old page. Subsequent turns use
    // this window's exact measured boundaries in both directions.
    std::vector<uint32_t> earlier;earlier.reserve(BACKTRACK_PAGES);
    size_t at=safeStart(text,length_,uint32_t(start>BACKTRACK_BYTES?start-BACKTRACK_BYTES:0));
    for(size_t count=0;count+1<BACKTRACK_PAGES;++count){
      const size_t following=next(at);
      if(following<=at||following>length_)return false;
      earlier.push_back(uint32_t(at));
      if(following>=start){
        visible.swap(earlier);selected=uint32_t(visible.size()-1);
        windowEnd_=following;return true;
      }
      at=following;
    }
    // Reserve one final layout call for a near-anchor seed when sparse lines
    // exhaust the larger lookbehind window. No scan from the book's beginning.
    at=safeStart(text,length_,uint32_t(start-1));
    const size_t following=next(at);
    if(at>=start||following<start||following<=at||following>length_)return false;
    visible.assign(1,uint32_t(at));selected=0;windowEnd_=following;return true;
  }

 private:
  std::vector<uint32_t> canonical_;
  size_t length_=0,cursor_=0,windowEnd_=0;
  bool active_=false,complete_=false,failed_=false,provisional_=false;
};
} // namespace progressive_index
