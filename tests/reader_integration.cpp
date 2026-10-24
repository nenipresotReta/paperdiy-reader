// Compile the actual reader, replacing only hardware and NVS boundaries.
#include <cassert>
#include <vector>
#include <array>
#include <tuple>
#include <type_traits>
#include <set>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define TEST_CONTENT_ASAN 1
#endif
#endif
#include "native_font_data.h"
#include "../src/reader.cpp"

const reader_font::Font& bodyNativeFont(){return reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
const lgfx::IFont* bodyFont(){static lgfx::IFont font;return &font;}
const lgfx::IFont* readerFont(){return bodyFont();}
static size_t simulatedGlyphCalls=0,glyphWorkPeriod=0;
static uint32_t simulatedWorkMs=0;
int bodyGlyphAdvance(uint32_t cp){
 ++simulatedGlyphCalls;
 if(glyphWorkPeriod&&simulatedGlyphCalls%glyphWorkPeriod==0){++testClock;++simulatedWorkMs;}
 return reader_font::glyphAdvance((reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28),cp);
}
const reader_font::Glyph* bodyGlyph(uint32_t cp){return reader_font::findGlyph((reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28),cp);}
uint32_t readerCodepoint(const char* text,size_t bytes){return reader_font::decodeCodepoint(text,bytes);}
using Mark=std::tuple<std::string,int,int>;
static std::vector<Mark> bodyMarks;
using Ink=std::tuple<int,int,int,int,uint32_t>;
static std::vector<Ink> inkMarks;
void recordInk(int x,int y,int w,int h,uint32_t color){inkMarks.emplace_back(x,y,w,h,color);}
void drawBodyGlyph(const char* text,size_t size,int x,int y,uint16_t){bodyMarks.emplace_back(std::string(text,size),x,y);}
void drawReaderGlyph(const char*,size_t,int,int,uint16_t){}
int readerGlyphWidth(uint32_t cp){return bodyGlyphAdvance(cp);}
bool bodyFontHasGlyph(uint32_t){return true;}

static std::vector<char> bytes;
void setBook(const std::string& text){
 reader_content::clear();cancelReaderIndex();
 bytes.assign(text.begin(),text.end());bytes.push_back(0);
 book=bytes.data();bookLength=text.size();bookIdentity="/test.epub";
 bookmarkKey="slegacy";pageOffsets.clear();pageNumber=0;
}
void reset(const std::string& text){
 prefs=Preferences{};assert(initializeProgress());setBook(text);
}
void assertContains(uint32_t offset){
 assert(!pageOffsets.empty());
 assert(pageOffsets[pageNumber]<=offset);
 assert(pageNumber+1==pageOffsets.size()||offset<pageOffsets[pageNumber+1]);
}
static_assert(std::is_const<decltype(reader_typography::style)>::value,"Standalone default style remains immutable");
void progressiveIndexRegression(){
 std::string source;
 for(unsigned i=0;i<500;++i){
  source+="Heading "+std::to_string(i)+"\n";
  source+="Paragraph "+std::to_string(i)+". Internationalization allows different line lengths. 中文正文的阅读位置需要保持。\n";
  source+="  code_value = "+std::to_string(i)+";  // Keep source spacing.\n\n";
 }
 for(unsigned mode=0;mode<3;++mode){
  clearMarkdown();bookDocument.clear();bookMarkdown=mode==2;bookMarkupFallback=false;
  bookEnglishHyphens=true;bookLanguage=text_language::Language::EnglishGB;reset(source);
  if(mode==1){
   epub::structure::Collector structure;structure.begin();const auto language=structure.internLanguage("en-GB");
   const auto unsupported=structure.internLanguage("fr");
   for(size_t at=0,line=0;at<source.size();++line){
    const size_t end=source.find('\n',at)+1;
    const bool heading=source.compare(at,8,"Heading ")==0,code=source.compare(at,7,"  code_")==0;
    const auto kind=heading?epub::structure::Kind::Heading:code?epub::structure::Kind::Preformatted:epub::structure::Kind::Paragraph;
    const auto lang=line%17?language:unsupported;
    const auto block=structure.open(kind,at,epub::structure::NONE,epub::structure::NONE,0,heading?2:0,lang);
    structure.text(at,end,block,epub::structure::NONE,0,0,lang);
    structure.close(block,end);at=end;
   }
   structure.finish(book,bookLength);
   assert(bookDocument.assign(structure,book,bookLength));
  }
  simulatedGlyphCalls=0;assert(indexBook());const auto expected=pageOffsets;
  const size_t fullCalls=simulatedGlyphCalls;assert(expected.size()>40);
  const uint32_t anchor=uint32_t(source.find("Internationalization",source.size()*4/5)+7);
  const auto key=bookmarkKey;prefs.values[key]=anchor;prefs.last=bookIdentity;
  const auto saved=prefs.values;const unsigned writes=prefs.writes;
  simulatedGlyphCalls=0;assert(indexBook(false,true,nullptr,false,true));
  assert(simulatedGlyphCalls<fullCalls/10); // Far anchors do not scan the prefix.
  assert(readerIndexPending()&&readerHasProvisionalPages()&&!readerIndexComplete()&&!readerIndexFailed());
  assert(!canonicalReaderPages()&&readerTotalPages()==0&&readerPageNumber()==0);
  assert(currentReadingOffset()==anchor&&pageOffsets[pageNumber]<=anchor&&currentPageEnd()>anchor);
  // A setting/source change cannot continue an old job with a mixed layout,
  // publish an old page range, or accidentally persist the wrong book's anchor.
  reader_settings::current().justify=!reader_settings::current().justify;
  assert(!advanceReaderIndex()&&!readerIndexComplete()&&!canonicalReaderPages());
  assert(currentPageEnd()==UINT32_MAX&&currentReadingOffset()==UINT32_MAX&&!turnReaderPage(1)&&!saveReadingProgress());
  reader_settings::current().justify=!reader_settings::current().justify;
  bookmarkKey="another";assert(!advanceReaderIndex()&&!saveReadingProgress());bookmarkKey=key;
  assert(readerIndexPending()&&currentReadingOffset()==anchor&&prefs.values==saved&&prefs.writes==writes);
  assert((uint8_t(book[pageOffsets[pageNumber]])&0xc0)!=0x80);
  bodyMarks.clear();assert(layoutPage(pageOffsets[pageNumber],true)==currentPageEnd());
  const auto originalMarks=bodyMarks;const auto originalView=pageOffsets;
  const auto originalPage=pageNumber;const auto originalEnd=currentPageEnd();
  // Checkpoint recovery extends only canonical work; the exact NVS anchor,
  // page body and unknown whole-book total remain unchanged until completion.
  const uint32_t splitUtf8[]={0,uint32_t(source.find("中文")+1)};
  assert(splitUtf8[1]<expected[2]&&!restoreReaderIndex(splitUtf8,2,expected[2]));
  const size_t recoveredCount=expected.size()/2;
  assert(restoreReaderIndex(expected.data(),recoveredCount,expected[recoveredCount]));
  assert(partialReaderPages()->size()==recoveredCount&&partialReaderCursor()==expected[recoveredCount]);
  assert(!canonicalReaderPages()&&readerTotalPages()==0&&currentReadingOffset()==anchor);
  assert(pageOffsets==originalView&&pageNumber==originalPage&&currentPageEnd()==originalEnd);
  assert(!restoreReaderIndex(expected.data(),expected.size(),uint32_t(bookLength)));
  while(readerIndexPending())assert(advanceReaderIndex(2,12));
  assert(readerIndexComplete()&&!readerIndexFailed()&&readerHasProvisionalPages());
  assert(*canonicalReaderPages()==expected&&readerTotalPages()==expected.size()&&readerPageNumber()==0);
  assert(pageOffsets==originalView&&pageNumber==originalPage&&currentPageEnd()==originalEnd&&currentReadingOffset()==anchor);
  bodyMarks.clear();assert(layoutPage(pageOffsets[pageNumber],true)==originalEnd&&bodyMarks==originalMarks);
  assert(saveReadingProgress()&&prefs.values==saved&&prefs.writes==writes);
  assert(turnReaderPage(1)&&!readerHasProvisionalPages());
  assert(pageOffsets==expected&&pageOffsets[pageNumber]<=originalEnd&&currentPageEnd()>originalEnd);
  assert(currentReadingOffset()==pageOffsets[pageNumber]&&readerPageNumber()==pageNumber+1);
  assert(saveReadingProgress()&&prefs.values[key]==pageOffsets[pageNumber]);

  // Before the prefix reaches this anchor, both directions work locally. The
  // first next page starts exactly at the measured end of the current page.
  prefs.values[key]=anchor;assert(indexBook(false,true,nullptr,false,true));
  const auto first=pageOffsets[pageNumber],end=currentPageEnd();
  assert(turnReaderPage(1)&&pageOffsets[pageNumber]==end);
  assert(turnReaderPage(-1)&&pageOffsets[pageNumber]==first);
  assert(turnReaderPage(-1)&&pageOffsets[pageNumber]<first&&currentPageEnd()>=first);
  const auto priorEnd=currentPageEnd();
  assert(turnReaderPage(1)&&pageOffsets[pageNumber]==priorEnd);
  assert(currentReadingOffset()==pageOffsets[pageNumber]&&saveReadingProgress());
  const auto localAnchor=currentReadingOffset();
  while(readerIndexPending())assert(advanceReaderIndex());
  assert(currentReadingOffset()==localAnchor&&saveReadingProgress()&&prefs.values[key]==localAnchor);

  // Cancellation before a complete cache load keeps the established cached
  // path unchanged; synchronous reflow still returns the identical page table.
  cancelReaderIndex();pageOffsets=expected;pageNumber=3;retainReadingAnchor(expected[3]);
  assert(readerIndexComplete()&&!readerIndexPending()&&!readerHasProvisionalPages());
  assert(canonicalReaderPages()==&pageOffsets&&readerTotalPages()==expected.size());
  assert(currentPageEnd()==expected[4]&&turnReaderPage(-1)&&pageNumber==2);
  assert(indexBook(false,true,nullptr,false)&&pageOffsets==expected&&!readerIndexPending());
  pageNumber=uint32_t(pageOffsets.size()-1);retainReadingAnchor(pageOffsets[pageNumber]);
  assert(currentPageEnd()==bookLength&&!turnReaderPage(1));
  assert(turnReaderPage(-1)&&currentPageEnd()==expected.back());
  // Opening the very last source bytes needs no prefix scan and does not
  // manufacture a next page while the canonical total is still unknown.
  prefs.values[key]=uint32_t(bookLength-2);
  assert(indexBook(false,true,nullptr,false,true)&&currentReadingOffset()==bookLength-2);
  assert(currentPageEnd()==bookLength&&!turnReaderPage(1));
  const auto finalStart=pageOffsets[pageNumber];
  assert(turnReaderPage(-1)&&pageOffsets[pageNumber]<finalStart&&currentPageEnd()>=finalStart);
 }
 clearMarkdown();bookDocument.clear();bookMarkdown=false;bookLanguage=text_language::Language::EnglishUS;
 cancelReaderIndex();bodyMarks.clear();
}
class WindowedFixture final:public reader_content::Source {
 std::string complete;char* destination;size_t first_=0,last_=0;
 public:
 bool allow=true;
 WindowedFixture(const std::string& value,char* target):complete(value),destination(target){}
 ~WindowedFixture(){
#ifdef TEST_CONTENT_ASAN
  __asan_unpoison_memory_region(destination,complete.size());
#endif
 }
 size_t begin()const override{return first_;}size_t end()const override{return last_;}size_t length()const override{return complete.size();}
 void restoreSpan(size_t first,size_t last)override{
  first_=first;last_=last;
#ifdef TEST_CONTENT_ASAN
  __asan_poison_memory_region(destination,complete.size());
  __asan_unpoison_memory_region(destination+first_,last_-first_);
#endif
  memcpy(destination+first_,complete.data()+first_,last_-first_);
 }
 bool pin(size_t first,size_t last)override{
  if(!allow||first>=last||last>complete.size())return false;
  first_=first/8192?first/8192*8192-8192:0;
  last_=std::min(complete.size(),((last+8191)/8192+1)*8192);
  while(first_<complete.size()&&(uint8_t(complete[first_])&0xc0)==0x80)++first_;
  while(last_<complete.size()&&(uint8_t(complete[last_])&0xc0)==0x80)--last_;
#ifdef TEST_CONTENT_ASAN
  __asan_poison_memory_region(destination,complete.size());
  __asan_unpoison_memory_region(destination+first_,last_-first_);
#endif
  memcpy(destination+first_,complete.data()+first_,last_-first_);return first_<=first&&last_>=last;
 }
};
void windowedContentRegression(){
 std::string source;for(unsigned i=0;i<1300;++i)source+="A synthetic paragraph demonstrates bounded text access and stable page composition. 中文测试正文。\n";
 for(unsigned semantic=0;semantic<4;++semantic){
  reader_content::clear();bookDocument.clear();bookMarkdown=false;bookMarkupFallback=false;bookEnglishHyphens=true;
  reset(source);bookLanguage=text_language::Language::EnglishUS;
  const size_t transition=source.find('\n',source.size()/2)+1;
  if(semantic){epub::structure::Collector collector;collector.begin();const auto lang=collector.internLanguage("en-US");
   auto block=[&](size_t first,size_t last,bool vertical){const uint16_t flags=vertical?epub::structure::VerticalRL:0;
    auto id=collector.open(epub::structure::Kind::Paragraph,first,epub::structure::NONE,epub::structure::NONE,flags,0,lang);
    collector.text(first,last,id,epub::structure::NONE,flags,0,lang);collector.close(id,last);};
   if(semantic==3){block(0,transition,false);block(transition,bookLength,true);}else block(0,bookLength,semantic==2);
   collector.finish(book,bookLength);assert(bookDocument.assign(collector,book,bookLength));}
  assert(indexBook());const auto pages=pageOffsets;
  std::vector<size_t> selected={pages.size()*4/5,0,pages.size()-2,pages.size()/3};
  if(semantic==3){const auto at=std::lower_bound(pages.begin(),pages.end(),transition);assert(at!=pages.end()&&*at==transition);selected.push_back(size_t(at-pages.begin()-1));selected.push_back(size_t(at-pages.begin()));}
  std::vector<std::vector<Mark>> expected;std::vector<size_t> ends;
  std::vector<std::vector<Ink>> expectedInk;M5.Display.observe=recordInk;
  for(auto page:selected){bodyMarks.clear();inkMarks.clear();ends.push_back(layoutPage(pages[page],true));expected.push_back(bodyMarks);expectedInk.push_back(inkMarks);}
  auto* fixture=new WindowedFixture(source,book);reader_content::install(std::unique_ptr<reader_content::Source>(fixture));
  for(size_t i=0;i<selected.size();++i){
   pageNumber=uint32_t(selected[i]);retainReadingAnchor(pages[pageNumber]+5);const auto anchor=currentReadingOffset();
   bodyMarks.clear();inkMarks.clear();assert(layoutPage(pages[pageNumber],true)==ends[i]&&bodyMarks==expected[i]&&inkMarks==expectedInk[i]);
   assert(readerContentLimit()-reader_content::begin()<source.size()/2&&currentReadingOffset()==anchor);
   const auto before=pageNumber;fixture->allow=false;
   assert(!turnReaderPage(1)&&pageNumber==before&&currentReadingOffset()==anchor);
   fixture->allow=true;
   const auto oldFirst=reader_content::begin(),oldLast=readerContentLimit();
   const size_t broken=source.find("中",pages[before+1])+1;assert(broken<source.size());
   pageOffsets[before+1]=uint32_t(broken);
   assert(!turnReaderPage(1)&&pageNumber==before&&currentReadingOffset()==anchor);
   assert(reader_content::begin()==oldFirst&&readerContentLimit()==oldLast); // Rejected successful pin rolls back its view.
   pageOffsets[before+1]=pages[before+1];
   assert(turnReaderPage(1)&&pageNumber==before+1&&currentReadingOffset()==pages[pageNumber]);
  }
  M5.Display.observe=nullptr;reader_content::clear();bookDocument.clear();
 }
}
void managedProgressRegression(){
 clearMarkdown();bookDocument.clear();reader_content::clear();prefs=Preferences{};assert(initializeProgress());
 for(unsigned i=0;i<49;++i)prefs.values["ordinary-"+std::to_string(i)]=100+i*31;
 const auto ordinary=prefs.values;
 assert(managed_progress::slot("/.paperdiy/lan/push0.md")==0&&managed_progress::slot("/.paperdiy/lan/push1.md")==1);
 for(const char* bad:{"push0.md","/.paperdiy/lan/push2.md","/.paperdiy/lan/push0.md/","/.paperdiy/lan/../push0.md","/.paperdiy/lan/Push0.md"})assert(!managed_progress::isPushPath(bad));
 std::string latest[2];
 auto open=[&](unsigned slot,const std::string& source){
  clearMarkdown();setBook(source);bookIdentity=slot?"/.paperdiy/lan/push1.md":"/.paperdiy/lan/push0.md";
  bookMarkdown=true;bookMarkupFallback=false;bookEnglishHyphens=true;bookLanguage=text_language::Language::EnglishUS;
  return indexBook(false,true,nullptr,true);
 };
 for(unsigned revision=0;revision<100;++revision){
  const unsigned slot=revision%2;
  std::string source="# Revision "+std::to_string(revision)+"\n\n";
  for(unsigned line=0;line<90;++line)source+="Synthetic rolling document retains bounded reading progress.\n";
  assert(open(slot,source)&&currentReadingOffset()==0);
  assert(bookmarkKey==managed_progress::key(slot));
  const uint32_t offset=uint32_t(source.size()/2+revision);
  pageNumber=uint32_t(std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset)-pageOffsets.begin()-1);
  retainReadingAnchor(offset);assert(saveReadingProgress());
  assert(prefs.values==ordinary&&prefs.blobs.size()==std::min(revision+1,2u));
  assert(prefs.last==bookIdentity&&prefs.blobs[managed_progress::key(slot)].size()==managed_progress::RECORD_BYTES);
  assert(open(slot,source)&&currentReadingOffset()==offset); // Same bytes resume exactly.
  const auto before=prefs.writes;assert(saveReadingProgress()&&prefs.writes==before);
  latest[slot]=source;
 }
 assert(prefs.values==ordinary&&prefs.blobs.size()==2);
 const auto intact=prefs.blobs;
 // Atomic write failures preserve the previous revision, then retry cleanly.
 assert(turnReaderPage(1));const auto moved=currentReadingOffset();prefs.failWrite=true;
 assert(!saveReadingProgress()&&progressSaveFailed&&prefs.blobs==intact&&prefs.values==ordinary);
 prefs.failWrite=false;assert(saveReadingProgress()&&!progressSaveFailed);
 assert(open(1,latest[1])&&currentReadingOffset()==moved);
 const auto good=prefs.blobs["pushRead1"];
 for(size_t at:{size_t(0),size_t(8),size_t(12),size_t(44)}){
  prefs.blobs["pushRead1"]=good;prefs.blobs["pushRead1"][at]^=1;const auto before=prefs.blobs;
  assert(!open(1,latest[1])&&prefs.blobs==before&&prefs.values==ordinary);
 }
 prefs.blobs["pushRead1"]=good;prefs.failReadKey="pushRead1";assert(!open(1,latest[1]));prefs.failReadKey.clear();
 prefs.blobs["pushRead1"].resize(good.size()-1);assert(!open(1,latest[1]));prefs.blobs["pushRead1"]=good;
 assert(!open(1,std::string(managed_progress::MAX_BYTES+1,'x'))&&prefs.values==ordinary);
 clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
}
void workBudgetRegression(){
 // Model a 10 ms RTOS sleep and 30 seconds of many sub-8 ms pages. The same
 // work budget spans every page and measures from the actual wake-up time.
 auto simulate=[](uint32_t interval,uint32_t start){
  uint32_t clock=start;unsigned sleeps=0;reader_work::Budget budget(clock,interval);
  for(unsigned page=0;page<5000;++page)for(unsigned work=0;work<6;++work){
   ++clock;budget.cooperate([&](){return clock;},[&](){clock+=10;++sleeps;});
  }
  assert(uint32_t(clock-start)==30000+10*sleeps);return sleeps;
 };
 assert(simulate(reader_work::INDEX_INTERVAL_MS,0)==600);
 assert(simulate(reader_work::DRAW_INTERVAL_MS,0)==3750);
 assert(simulate(reader_work::INDEX_INTERVAL_MS,UINT32_MAX-20)==600);
 uint32_t clock=0;unsigned sleeps=0;reader_work::Budget budget(clock,50);
 auto poll=[&](){return budget.cooperate([&](){return clock;},[&](){clock+=10;++sleeps;});};
 clock=49;assert(!poll());clock=50;assert(poll()&&clock==60);
 assert(!poll());clock=109;assert(!poll());clock=110;assert(poll());
 clock+=500;assert(poll()&&sleeps==3&&!poll()); // No catch-up sleep burst.

 // Exercise actual indexBook propagation for all adapters. Make each whole
 // page take at most 6 simulated ms: a per-page reset would never yield.
 std::string source;for(unsigned i=0;i<12000;++i)source+="中";
 for(unsigned mode=0;mode<3;++mode){
  clearMarkdown();bookDocument.clear();bookMarkdown=mode==1;bookMarkupFallback=false;bookEnglishHyphens=true;
  reset(source);
  if(mode==2){
   epub::structure::Collector structure;structure.begin();
   const auto language=structure.internLanguage("en-US");
   const auto paragraph=structure.open(epub::structure::Kind::Paragraph,0,epub::structure::NONE,epub::structure::NONE,0,0,language);
   structure.text(0,bookLength,paragraph,epub::structure::NONE,0,0,language);
   structure.close(paragraph,bookLength);structure.finish(book,bookLength);
   assert(bookDocument.assign(structure,book,bookLength));
  }
  assert(indexBook());const auto expected=pageOffsets;assert(expected.size()>20);
  size_t maxPageCalls=0;
  for(auto at:expected){
   const auto before=simulatedGlyphCalls;layoutPage(at,false);
   maxPageCalls=std::max(maxPageCalls,simulatedGlyphCalls-before);
  }
  assert(maxPageCalls>0);glyphWorkPeriod=(maxPageCalls+5)/6;
  simulatedGlyphCalls=0;simulatedWorkMs=0;const uint32_t begin=testClock;
  assert(indexBook());const uint32_t slept=testClock-begin-simulatedWorkMs;
  assert(pageOffsets==expected&&simulatedWorkMs>100);
  assert(slept>0&&slept<=simulatedWorkMs/reader_work::INDEX_INTERVAL_MS+1);
  glyphWorkPeriod=0;
 }
 clearMarkdown();bookDocument.clear();bookMarkdown=false;
}
void typographyRegression(){
 std::string source;
 for(unsigned i=0;i<160;++i)source+="她说：“《春日》很好。”中文EPUB3第12章。**原文** 中 A 。 ";
 assert(reader_typography::style.flags()==3);
 for(bool markdown:{false,true}){
  clearMarkdown();bookMarkdown=markdown;bookMarkupFallback=false;bookEnglishHyphens=true;
  reset(source);const std::string key=contentKey(false);
  const uint32_t exact=uint32_t(source.find("中文",source.size()/2));assert(exact!=UINT32_MAX);
  prefs.values[key]=exact;prefs.last=bookIdentity;
  // A legacy disabled preference cannot alter the fixed production style.
  prefs.values["cnStyle"]=0;
  const auto original=prefs.values;const unsigned writes=prefs.writes;
  std::vector<uint32_t> baseline;
  for(unsigned repeat=0;repeat<3;++repeat){
   pageOffsets.clear();assert(indexBook());assert(bookmarkKey==key&&currentReadingOffset()==exact);
   assertContains(exact);if(!repeat)baseline=pageOffsets;else assert(pageOffsets==baseline);
   assert(prefs.values==original&&prefs.writes==writes&&reader_typography::style.flags()==3);
   for(size_t i=0;i<pageOffsets.size();++i){
    const size_t from=pageOffsets[i],end=i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength;
    bodyMarks.clear();assert(layoutPage(from,true)==end);
   }
   assert(currentReadingOffset()==exact&&saveReadingProgress());
   assert(prefs.values==original&&prefs.writes==writes);
   assert(bookLength==source.size()&&!memcmp(book,source.data(),source.size()));
  }
 }
 clearMarkdown();bookMarkdown=false;bodyMarks.clear();
}
void settingsReflowRegression(){
 std::string source;for(unsigned i=0;i<400;++i)source+="中文Markdown3与标点：“hello.” Internationalization allows different line lengths.\n";
 for(bool markdown:{false,true}){
  reader_settings::current()={};bookMarkdown=markdown;bookMarkupFallback=false;bookEnglishHyphens=true;reset(source);
  const auto key=contentKey(false);const auto exact=uint32_t(source.find("Internationalization",source.size()/2));prefs.values[key]=exact;
  std::set<size_t> totals;
  for(uint8_t preset:{uint8_t(0),uint8_t(1),uint8_t(2)}){
   auto& c=reader_settings::current();c.preset=preset;c.spacing=preset==0?2:4;c.hyphenation=preset!=0;c.justify=preset!=0;
   assert(indexBook());assertContains(exact);assert(currentReadingOffset()==exact);totals.insert(pageOffsets.size());
   assert(saveReadingProgress()&&prefs.values[key]==exact);
   for(auto offset:pageOffsets){const auto end=layoutPage(offset,false);bodyMarks.clear();assert(layoutPage(offset,true)==end);}
  }
  assert(totals.size()==3);assert(!memcmp(book,source.data(),source.size()));
 }
 reader_settings::current()={};clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
}
int main(){
 managedProgressRegression();
 windowedContentRegression();
 progressiveIndexRegression();
 workBudgetRegression();
 settingsReflowRegression();
 typographyRegression();
 std::string text;
 for(int i=0;i<300;++i)text+="Paragraph "+std::to_string(i)+". Enough text to wrap into stable pages. 中文段落。\n";
 reset(text);const std::string current=contentKey(false),legacy=contentKey(true);
 assert(current[0]=='c'&&legacy[0]=='b');

 // Prefer the existing current-format bookmark over every migration source.
 prefs.values[current]=4500;prefs.values[legacy]=1200;prefs.values["slegacy"]=1;
 auto anchor=bookmarks::capture(text.data(),text.size(),2100);
 assert(indexBook(false,true,&anchor,true));assertContains(4500);
 assert(saveReadingProgress());assert(prefs.values[legacy]==1200&&prefs.values["slegacy"]==1);
 assert(prefs.values[current]==4500); // Opening/reflow must retain the exact source anchor.
 assert(prefs.last=="/test.epub");

 // A same-text legacy key migrates without deleting or overwriting the old key.
 reset(text);prefs.values[legacy]=3200;
 assert(indexBook(false,true,nullptr,true));assertContains(3200);
 assert(prefs.values.find(current)==prefs.values.end()); // Indexing is read-only.
 assert(saveReadingProgress());assert(prefs.values[legacy]==3200);
 assert(prefs.values[current]==3200);

 // Translate changed extraction text using old context; persist only the new key.
 reset(text);uint32_t oldOffset=text.find("Paragraph 150.");
 anchor=bookmarks::capture(text.data(),text.size(),oldOffset);
 prefs.values[legacy]=oldOffset;
 std::string changed="New introduction.\n"+text;
 for(size_t p=0;(p=changed.find("to wrap",p))!=std::string::npos;p+=8)changed.insert(p," ");
 setBook(changed);std::string changedKey=contentKey(false);
 uint32_t mapped=0;assert(bookmarks::relocate(anchor,changed.data(),changed.size(),mapped,[](){}));
 assert(indexBook(false,true,&anchor,true));assertContains(mapped);
 assert(saveReadingProgress());assert(prefs.values[legacy]==oldOffset);
 assert(prefs.values[changedKey]==mapped);

 // An unreadable cache requires recovery; never manufacture a new bookmark at 0.
 reset(changed);prefs.values[legacy]=oldOffset;
 auto before=prefs.values;
 assert(!indexBook(false,true,nullptr,true));
 assert(pageOffsets.empty()&&prefs.values==before&&prefs.writes==0);
 assert(!saveReadingProgress()&&prefs.values==before);
 setBook(changed);auto missing=bookmarks::capture("unknown passage",15,4);
 assert(!indexBook(false,true,&missing,true)&&prefs.values==before&&prefs.writes==0);

 // Fail closed when a known current, legacy, or page key cannot be read.
 for(const auto& key:std::vector<std::string>{current,legacy,"slegacy"}){
   reset(text);prefs.values[key]=2;prefs.failReadKey=key;before=prefs.values;
   assert(!indexBook(false,true,nullptr,true));
   assert(pageOffsets.empty()&&prefs.values==before&&prefs.writes==0);
 }

 // A failed migration write remains retryable and preserves the source bookmark.
 reset(text);prefs.values[legacy]=3200;
 assert(indexBook(false,true,nullptr,true));prefs.failWrite=true;
 assert(!saveReadingProgress()&&progressSaveFailed);
 assert(prefs.values[legacy]==3200&&prefs.values.find(current)==prefs.values.end());
 prefs.failWrite=false;prefs.failResume=true;
 assert(!saveReadingProgress()&&progressSaveFailed&&prefs.last.empty());
 prefs.failResume=false;
 assert(saveReadingProgress()&&!progressSaveFailed&&prefs.last=="/test.epub");
 assert(prefs.values[legacy]==3200);

 // Truly new books may start at zero; historical page keys remain read-only.
 reset(text);assert(indexBook(false,true,nullptr,false)&&pageNumber==0);
 reset(text);prefs.values["slegacy"]=2;
 assert(indexBook(false,true,nullptr,true));assert(pageNumber>0);
 assert(saveReadingProgress()&&prefs.values["slegacy"]==2);
 // A corrupt/outdated offset must not be silently rewritten to an endpoint.
 for(uint32_t invalid : {uint32_t(text.size()),UINT32_MAX-1}){
   reset(text);prefs.values[current]=invalid;auto original=prefs.values;
   assert(!indexBook(false,true,nullptr,false));
   assert(pageOffsets.empty()&&prefs.values==original&&prefs.writes==0);
   assert(!saveReadingProgress()&&prefs.values==original);
 }
 // A previously saved raw-source offset must survive rebuilding from a
 // plain-text fallback layout into Markdown, including a UTF-8 BOM and syntax
 // that disappears from the rendered buffer. Both source extensions retain
 // independent book identities and the exact original bytes.
 std::string markdown="\xef\xbb\xbf# 阅读笔记\n\n";
 for(int i=0;i<90;++i){
   markdown+="## Section "+std::to_string(i)+"\n\n";
   markdown+="- **Chinese 中文** and *emphasis* with `code` and $x_1$. ";
   markdown+="A paragraph with enough words to wrap across multiple lines.\n\n";
 }
 std::string firstMarkdownKey;
 for(const std::string& path:{std::string("/notes.md"),std::string("/notes.markdown")}){
   clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;reset(markdown);
   bookIdentity=path;const std::string rawKey=contentKey(false);
   if(firstMarkdownKey.empty())firstMarkdownKey=rawKey;
   else assert(rawKey!=firstMarkdownKey);
   assert(indexBook(false,true,nullptr,false)&&pageOffsets.size()>3);
   const auto plainPages=pageOffsets;
   pageNumber=uint32_t(pageOffsets.size()/2);
   const uint32_t rawSaved=pageOffsets[pageNumber];
   assert(saveReadingProgress()&&prefs.values[rawKey]==rawSaved&&prefs.last==path);
   const auto saved=prefs.values;const unsigned writes=prefs.writes;

   pageOffsets.clear();pageNumber=0;bookMarkdown=true;
   assert(indexBook(false,true,nullptr,true));
   assert(bookMarkdown&&!bookMarkupFallback&&bookmarkKey==rawKey);
   assert(pageOffsets!=plainPages);assertContains(rawSaved);
   assert(bookLength==markdown.size()&&!memcmp(book,markdown.data(),bookLength));
   assert(prefs.values==saved&&prefs.writes==writes&&prefs.last==path);
   // Every rebuilt page boundary is a raw-source UTF-8 boundary, and using
   // that boundary in the production renderer reaches exactly the next page.
   for(size_t i=0;i<pageOffsets.size();++i){
     const size_t start=pageOffsets[i];assert(start<bookLength);
     assert((uint8_t(book[start])&0xc0)!=0x80);
     const size_t end=i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength;
     assert(layoutPage(start,false)==end&&end>start);
   }
   assert(saveReadingProgress());const uint32_t exactRaw=rawSaved;
   const uint32_t expectedPage=pageNumber;const auto rebuiltPages=pageOffsets;
   const auto rebuiltSaved=prefs.values;const unsigned rebuiltWrites=prefs.writes;
   assert(rebuiltSaved.at(rawKey)==exactRaw);
   clearMarkdown();pageOffsets.clear();pageNumber=0;
   assert(indexBook(false,true,nullptr,true)&&pageNumber==expectedPage);
   assert(pageOffsets==rebuiltPages);assertContains(exactRaw);
   assert(bookmarkKey==rawKey&&prefs.values==rebuiltSaved&&prefs.writes==rebuiltWrites);
   assert(bookLength==markdown.size()&&!memcmp(book,markdown.data(),bookLength));
   // Reopening/reflowing repeatedly must not ratchet the source anchor backward.
   for(unsigned pass=0;pass<8;++pass){
     clearMarkdown();bookMarkdown=(pass%2)==0;pageOffsets.clear();
     assert(indexBook(false,true,nullptr,true));assertContains(exactRaw);
     const unsigned beforeSave=prefs.writes;
     assert(currentReadingOffset()==exactRaw&&saveReadingProgress());
     assert(prefs.values.at(rawKey)==exactRaw&&prefs.writes==beforeSave);
   }
   // A real page turn establishes its page start, including failure/retry and
   // turning back. It must not resurrect the pre-turn reflow anchor.
   const uint32_t previousPage=pageNumber;
   assert(pageNumber+1<pageOffsets.size());++pageNumber;
   const uint32_t nextStart=pageOffsets[pageNumber];
   prefs.failWrite=true;assert(!saveReadingProgress()&&progressSaveFailed);
   assert(prefs.values.at(rawKey)==exactRaw&&currentReadingOffset()==nextStart);
   prefs.failWrite=false;assert(saveReadingProgress()&&prefs.values.at(rawKey)==nextStart);
   pageNumber=previousPage;assert(saveReadingProgress());
   assert(prefs.values.at(rawKey)==pageOffsets[previousPage]);
 }
 clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
 puts("reader integration: progressive windows, canonical handoff, exact anchors, recovery precedence, legacy context, corrupt-cache guard, NVS faults, preserved old keys, Markdown raw-source rebuild/resume: PASS");
}
