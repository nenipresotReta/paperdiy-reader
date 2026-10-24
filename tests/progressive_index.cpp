#include "progressive_index.h"
#include <cassert>
#include <cstdio>
#include <string>

using progressive_index::Session;
int main(){
  std::string source(10000,' ');std::vector<uint32_t> visible;uint32_t page=0,clock=0;
  size_t calls=0;auto next=[&](size_t at){++calls;clock+=6;return std::min(source.size(),at+100);};
  Session index;
  assert(index.begin(source.size(),8503,visible,page,next));
  assert(calls==1&&visible==std::vector<uint32_t>{8503}&&page==0&&index.pageEnd(visible,page)==8603);
  assert(index.pending()&&!index.complete()&&index.provisional()&&!index.failed());
  assert(index.canonical().empty());
  assert(!index.advance(0,12,next,[&](){return clock;})&&!index.advance(2,0,next,[&](){return clock;}));
  assert(index.advance(20,12,next,[&](){return clock;})&&calls==3&&index.canonical().size()==2);
  const auto firstWindow=visible;
  assert(index.turn(1,source.data(),visible,page,next)&&visible[page]==8603);
  assert(index.turn(-1,source.data(),visible,page,next)&&visible[page]==8503);
  assert(visible[page]<index.pageEnd(visible,page));
  // Going past a deep window's left edge is bounded, even with sparse lines.
  calls=0;assert(index.turn(-1,source.data(),visible,page,next));
  assert(calls<=progressive_index::BACKTRACK_PAGES&&visible[page]<8503&&index.pageEnd(visible,page)>=8503);
  const auto beforeCompletion=visible;const auto beforePage=page;const auto beforeEnd=index.pageEnd(visible,page);
  while(index.pending())assert(index.advance(2,12,next,[&](){return clock;}));
  assert(index.complete()&&!index.failed()&&index.canonical().size()==100);
  for(size_t i=0;i<index.canonical().size();++i)assert(index.canonical()[i]==i*100);
  assert(visible==beforeCompletion&&page==beforePage&&index.pageEnd(visible,page)==beforeEnd);
  // Only a deliberate turn adopts canonical pages. Include the first unread
  // byte even when reflow produces a small overlap with the provisional page.
  assert(index.turn(1,source.data(),visible,page,next)&&!index.provisional());
  assert(visible.size()==100&&visible[page]<=beforeEnd&&index.pageEnd(visible,page)>beforeEnd);
  assert(!index.turn(1,source.data(),visible,page,next)); // Normal full-index navigation is the caller's job.

  index.reset();calls=0;visible.clear();
  assert(index.begin(source.size(),0,visible,page,next)&&index.canonical()==std::vector<uint32_t>{0});
  assert(index.advance(1,100,next,[&](){return clock;})&&index.canonical()==std::vector<uint32_t>({0,100}));
  assert(!index.turn(-1,source.data(),visible,page,next));
  for(unsigned i=0;i<40;++i)assert(index.turn(1,source.data(),visible,page,next));
  assert(visible.size()==progressive_index::WINDOW_PAGES&&visible[page]==4000);
  // Preserve the successfully measured local window after a background fault.
  const auto goodWindow=visible;const auto goodPage=page;const auto goodEnd=index.pageEnd(visible,page);
  assert(index.advance(2,12,[](size_t at){return at;},[&](){return clock;}));
  assert(index.failed()&&!index.pending()&&!index.complete());
  assert(visible==goodWindow&&page==goodPage&&index.pageEnd(visible,page)==goodEnd);
  assert(index.turn(1,source.data(),visible,page,next)&&visible[page]==goodEnd);
  assert(index.turn(-1,source.data(),visible,page,next)&&visible[page]==4000);

  // Unsigned elapsed-time accounting survives millis() wrapping.
  clock=UINT32_MAX-3;calls=0;
  assert(index.begin(source.size(),8000,visible,page,next));
  clock=UINT32_MAX-3;
  assert(index.advance(10,12,next,[&](){return clock;})&&index.canonical().size()==2);
  // Reject invalid layout ranges without publishing a broken page table.
  index.reset();visible={17};page=0;
  assert(!index.begin(source.size(),8000,visible,page,[](size_t){return 8000;}));
  assert(visible==std::vector<uint32_t>{17}&&!index.active());
  assert(!index.begin(0,0,visible,page,next));
  assert(index.begin(source.size(),9900,visible,page,next));
  assert(!index.turn(1,source.data(),visible,page,next));
  assert(index.advance(1,12,[&](size_t){return source.size()+1;},[&](){return clock;}));
  assert(index.failed()&&visible==std::vector<uint32_t>{9900});

  // A bounded page count also handles a malicious layout that advances one
  // byte per page without allowing the canonical vector to grow forever.
  index.reset();size_t length=progressive_index::MAX_PAGES+1;
  assert(index.begin(length,1000,visible,page,[](size_t at){return at+1;}));
  assert(index.advance(length+1,100,[](size_t at){return at+1;},[](){return 0u;}));
  assert(index.failed()&&index.canonical().size()==progressive_index::MAX_PAGES);

  // Recover a validated prefix without changing a deep local page. Continue
  // exactly at the saved canonical cursor, and never accept a complete/torn
  // prefix as a resumable partial product.
  assert(index.begin(source.size(),8503,visible,page,next));
  const auto recoveryView=visible;const auto recoveryEnd=index.pageEnd(visible,page);
  const uint32_t prefix[]={0,100,200,300};
  assert(index.restore(prefix,4,400)&&index.cursor()==400&&index.pending());
  assert(visible==recoveryView&&index.pageEnd(visible,page)==recoveryEnd);
  assert(!index.restore(prefix,4,400)&&!index.restore(prefix,4,uint32_t(source.size())));
  const uint32_t badPrefix[]={0,200,100};assert(!index.restore(badPrefix,3,500));
  assert(index.advance(1,12,next,[&](){return clock;})&&index.canonical().back()==400);
  const uint32_t conflicting[]={0,99,200,300,400,500};
  assert(!index.restore(conflicting,6,600));
  while(index.pending())index.advance(10,100,next,[&](){return clock;});
  assert(index.complete()&&index.canonical().size()==100&&visible==recoveryView);

  const std::string unicode="中文 Internationalization tail";
  assert(progressive_index::safeStart(unicode.data(),unicode.size(),2)==0);
  assert(progressive_index::safeStart(unicode.data(),unicode.size(),5)==3);
  const auto word=unicode.find("Internationalization");
  assert(progressive_index::safeStart(unicode.data(),unicode.size(),word+12)==word);
  const std::string hostile(2000,'A');
  assert(progressive_index::safeStart(hostile.data(),hostile.size(),1900)==1900-progressive_index::WORD_LOOKBEHIND);
  const std::string accents="before naïve re\xcc\x81sume after";
  assert(progressive_index::safeStart(accents.data(),accents.size(),accents.find("ve"))==accents.find("na"));
  assert(progressive_index::safeStart(accents.data(),accents.size(),accents.find("sume"))==accents.find("re\xcc"));
  puts("progressive index: saved-window first, bounded slices/backtracking, canonical handoff, range faults, UTF-8 anchors: PASS");
}
