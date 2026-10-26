#include "console.h"
#include "reader.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <mbedtls/sha256.h>
#include "reader_font.h"
#include "reading_font.h"
#include "bookmark_store.h"
#include "pagination.h"
#include "markdown_reader.h"
#include "epub_reader.h"
#include "reader_typography.h"
#include "progressive_index.h"
namespace reader_typography {const Style style;}
Preferences prefs;
char* book=nullptr;
size_t bookLength=0;
std::vector<uint32_t> pageOffsets;
uint32_t pageNumber=0;
String bookmarkKey="page",bookIdentity="uploaded";
bool progressSaveFailed=false;
bool bookEnglishHyphens=true;
text_language::Language bookLanguage=text_language::Language::EnglishUS;
bool bookMarkdown=false,bookMarkupFallback=false;
namespace {
bool progressAvailable=false;
uint32_t retainedOffset=0,retainedPage=UINT32_MAX;
char retainedKey[16]={};
progressive_index::Session progressiveIndex;
const char* indexedBook=nullptr;
size_t indexedLength=0;
String indexedKey;
uint32_t indexedStyle=0;
reader_work::Budget indexBudget(0,reader_work::INDEX_INTERVAL_MS);
uint32_t currentIndexStyle(){
  return reader_settings::layoutBits()|(uint32_t(reader_typography::currentStyle().flags())<<12)|
    (uint32_t(bookEnglishHyphens)<<14)|(uint32_t(bookMarkdown)<<15)|
    (uint32_t(bookLanguage)<<16)|(uint32_t(bookDocument.ready())<<20);
}
bool liveIndex(){
  return progressiveIndex.active()&&indexedBook==book&&indexedLength==bookLength&&indexedKey==bookmarkKey&&indexedStyle==currentIndexStyle();
}
String contentKey(bool legacy){
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
  uint32_t lastYield=millis(); size_t work=0;
  bookmarks::identity([&](const uint8_t* bytes,size_t count){
    mbedtls_sha256_update_ret(&sha,bytes,count);work+=count;
    if(work>=128u*1024 || uint32_t(millis()-lastYield)>=50){
      delay(1);work=0;lastYield=millis();
    }
  },bookIdentity.c_str(),bookIdentity.length(),book,bookLength,legacy);
  uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);
  char key[16];key[0]=legacy?'b':'c';
  for(int i=0;i<7;++i)snprintf(key+1+i*2,3,"%02x",digest[i]);
  return String(key);
}
}
bool initializeProgress(){
  // Retain the deployed namespace and all old keys for reversible migration.
  progressAvailable=prefs.begin("paperwifi",false);
  progressSaveFailed=!progressAvailable;
  if(!progressAvailable)Console.println("PROGRESS_FAILED operation=begin");
  return progressAvailable;
}
void retainReadingAnchor(uint32_t offset){
  retainedOffset=offset;retainedPage=pageNumber;
  strncpy(retainedKey,bookmarkKey.c_str(),sizeof(retainedKey)-1);
  retainedKey[sizeof(retainedKey)-1]=0;
}
uint32_t currentReadingOffset(){
  if(progressiveIndex.active()&&!liveIndex())return UINT32_MAX;
  if(pageOffsets.empty()||pageNumber>=pageOffsets.size())return UINT32_MAX;
  const uint32_t start=pageOffsets[pageNumber];
  const size_t end=currentPageEnd();
  // Reflow chooses a containing page, not a new reading position. Keep the
  // precise source anchor until navigation actually changes the page.
  if(retainedPage==pageNumber&&bookmarkKey==retainedKey&&
     retainedOffset>=start&&retainedOffset<end)return retainedOffset;
  return start;
}
void cancelReaderIndex(){
  progressiveIndex.reset();indexedBook=nullptr;indexedLength=0;indexedKey="";
}
bool readerIndexPending(){return liveIndex()&&progressiveIndex.pending();}
bool readerHasProvisionalPages(){return liveIndex()&&progressiveIndex.provisional();}
bool readerIndexFailed(){return liveIndex()&&progressiveIndex.failed();}
bool readerIndexComplete(){
  if(progressiveIndex.active())return liveIndex()&&progressiveIndex.complete();
  return !pageOffsets.empty()&&pageOffsets.front()==0&&pageOffsets.back()<bookLength;
}
const std::vector<uint32_t>* canonicalReaderPages(){
  if(!readerIndexComplete())return nullptr;
  return readerHasProvisionalPages()?&progressiveIndex.canonical():&pageOffsets;
}
const std::vector<uint32_t>* partialReaderPages(){
  return readerIndexPending()?&progressiveIndex.canonical():nullptr;
}
uint32_t partialReaderCursor(){return readerIndexPending()?progressiveIndex.cursor():0;}
bool restoreReaderIndex(const uint32_t* pages,size_t count,uint32_t cursor){
  if(!readerIndexPending()||!pagination_checkpoint::validPrefix(pages,count,cursor,bookLength)||
     progressive_index::continuation(book[cursor]))return false;
  for(size_t i=0;i<count;++i)if(progressive_index::continuation(book[pages[i]]))return false;
  return progressiveIndex.restore(pages,count,cursor);
}
uint32_t readerPageNumber(){
  if(progressiveIndex.active()&&!liveIndex())return 0;
  // A local window beginning at zero is itself a measured canonical prefix.
  return pageNumber<pageOffsets.size()&&(!readerHasProvisionalPages()||pageOffsets.front()==0)?pageNumber+1:0;
}
uint32_t readerTotalPages(){
  const auto* pages=canonicalReaderPages();return pages?uint32_t(pages->size()):0;
}
uint32_t currentPageEnd(){
  if(progressiveIndex.active()&&!liveIndex())return UINT32_MAX;
  if(pageNumber>=pageOffsets.size())return UINT32_MAX;
  return readerHasProvisionalPages()?uint32_t(progressiveIndex.pageEnd(pageOffsets,pageNumber)):
    pageNumber+1<pageOffsets.size()?pageOffsets[pageNumber+1]:uint32_t(bookLength);
}
bool advanceReaderIndex(unsigned maxPages,uint32_t maxMs){
  if(!readerIndexPending())return false;
  const bool changed=progressiveIndex.advance(maxPages,maxMs,
    [&](size_t at){return layoutPage(at,false,24,&indexBudget);},[](){return millis();});
  if(changed&&!progressiveIndex.pending())Console.printf("BOOK_INDEX status=%s pages=%u\n",
    progressiveIndex.complete()?"complete":"failed",unsigned(progressiveIndex.canonical().size()));
  return changed;
}
bool pinReaderPage(size_t selected){
  if(!reader_content::active())return true;
  const size_t previousFirst=reader_content::begin(),previousLast=reader_content::limit();
  auto rollback=[&](){reader_content::source()->restoreSpan(previousFirst,previousLast);return false;};
  const auto range=reader_content::pageRange(pageOffsets,selected,bookLength);
  if(!reader_content::pin(range.first,range.second))return false;
  if(progressive_index::continuation(book[pageOffsets[selected]])||
     (selected+1<pageOffsets.size()&&progressive_index::continuation(book[pageOffsets[selected+1]])))return rollback();
  if(bookDocument.ready()&&!bookDocument.validateSpan(reader_content::begin(),reader_content::limit())){
    if(!reader_content::pin(0,bookLength)||!bookDocument.validateSpan(0,bookLength))return rollback();
  }
  return true;
}
bool turnReaderPage(int direction){
  if(progressiveIndex.active()&&!liveIndex())return false;
  if(!direction||pageNumber>=pageOffsets.size())return false;
  if(readerHasProvisionalPages()){
    if(!progressiveIndex.turn(direction,book,pageOffsets,pageNumber,
        [&](size_t at){return layoutPage(at,false,24,&indexBudget);}))return false;
  }else if(direction<0&&pageNumber){if(!pinReaderPage(pageNumber-1))return false;--pageNumber;}
  else if(direction>0&&pageNumber+1<pageOffsets.size()){if(!pinReaderPage(pageNumber+1))return false;++pageNumber;}
  else return false;
  retainReadingAnchor(pageOffsets[pageNumber]);return true;
}
bool saveReadingProgress(){
  const uint32_t offset=currentReadingOffset();
  if(offset==UINT32_MAX)return false;
  // This is the current in-memory location even if persistence must be retried.
  retainReadingAnchor(offset);
  bookmarks::SaveResult result;
  const int managed=managed_progress::slot(bookIdentity.c_str());
  if(managed>=0){
    if(!bookLength||bookLength>managed_progress::MAX_BYTES)return false;
    uint8_t digest[32];mbedtls_sha256_ret((const uint8_t*)book,bookLength,digest,0);
    result=managed_progress::save(prefs,progressAvailable,managed,digest,bookLength,offset,bookIdentity.c_str(),bookIdentity.length());
  }else result=bookmarks::save(prefs,progressAvailable,bookmarkKey.c_str(),
    offset,bookIdentity.c_str(),bookIdentity.length());
  progressSaveFailed=result!=bookmarks::SaveResult::Saved;
  if(progressSaveFailed)Console.printf("PROGRESS_FAILED operation=%u\n",unsigned(result));
  return !progressSaveFailed;
}
size_t utf8Char(const char* s,size_t remain,char out[5]){
  size_t n=pagination::utf8Bytes(s,remain);memcpy(out,s,n);out[n]=0;return n;
}
int16_t asciiWidths[128];
void prepareReaderFont(){
  M5.Display.setFont(bodyFont());M5.Display.setTextSize(1);
  for(int i=0;i<128;++i)asciiWidths[i]=bodyGlyphAdvance(i);
  prepareEpubMetrics();
}
// Drawing and cache indexing execute the same layout implementation.
size_t layoutPage(size_t from,bool draw,int top,reader_work::Budget* sharedBudget){
  reader_work::Budget localBudget(millis(),draw?reader_work::DRAW_INTERVAL_MS:reader_work::INDEX_INTERVAL_MS);
  auto& budget=sharedBudget?*sharedBudget:localBudget;
  if(reader_content::active()){
    const auto at=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),from);
    if(at==pageOffsets.begin()||!pinReaderPage(size_t(at-pageOffsets.begin()-1)))return from;
  }
  if(bookMarkdown)return layoutMarkdown(from,draw,top,&budget);
  if(bookDocument.ready())return layoutEpub(from,draw,top,&budget);
  // The 30 px right margin is also the display-only hang budget; a full-width
  // closing mark at 510 still ends at or before 540.
  const auto shape=reader_settings::geometry();
  pagination::Geometry g;g.language=bookLanguage;g.left=shape.left;g.right=shape.right;g.hangLimit=540-g.right;
  g.top=top;g.lineHeight=shape.lineHeight;g.hyphenate=bookEnglishHyphens&&reader_settings::current().hyphenation;g.justify=reader_settings::current().justify;
  return reader_typography::page(book,std::min(bookLength,readerContentLimit()),from,g,reader_typography::currentStyle(),
    [](uint32_t cp){
      reader_typography::Metrics result;result.advance=cp<128?asciiWidths[cp]:bodyGlyphAdvance(cp);
      if(typography_experiment::punctuation(cp)!=typography_experiment::Punctuation::None)if(const auto* glyph=bodyGlyph(cp)){
        result.ink=glyph->width&&glyph->height;result.inkLeft=glyph->left;result.inkRight=glyph->left+glyph->width;
      }
      return result;
    },
    [draw](const char* ch,size_t n,int x,int y){if(draw)drawBodyGlyph(ch,n,x,y);},
    [&](){budget.cooperate([](){return millis();},[](){delay(1);});},draw);
}
namespace {
// One-shot context from the Push page that was open when P replaced it.
bookmarks::Anchor livePushAnchor;
}
void retainLivePushPosition(){
  livePushAnchor=bookmarks::Anchor();
  const uint32_t offset=currentReadingOffset();
  if(!book||offset==UINT32_MAX||offset>=bookLength)return;
  auto anchor=bookmarks::capture(book,bookLength,offset);
  // A short context survives edits near the page. The search still prefers
  // the match closest to the old relative position.
  constexpr size_t BEFORE=48,AFTER=96;
  if(anchor.cursor>BEFORE){anchor.context.erase(0,anchor.cursor-BEFORE);anchor.cursor=BEFORE;}
  if(anchor.context.size()>anchor.cursor+AFTER)anchor.context.resize(anchor.cursor+AFTER);
  anchor.valid=!anchor.context.empty();
  livePushAnchor=anchor;
}
void clearLivePushPosition(){livePushAnchor=bookmarks::Anchor();}
bool indexBook(bool reset,bool migrateLegacy,const bookmarks::Anchor* anchor,bool requireRecovery,bool progressive){
  cancelReaderIndex();
  if(reader_content::active()){
    reader_content::cancelPrefetch();
    if(!reader_content::pin(0,bookLength)||(bookDocument.ready()&&!bookDocument.validateSpan(0,bookLength)))return false;
    reader_content::clear();
  }
  if(!bookLength)return false;book[bookLength]=0;prepareReaderFont();
  if(bookMarkdown&&!prepareMarkdown(book,bookLength)){
    bookMarkdown=false;bookMarkupFallback=true;
    Console.println("MARKDOWN_FALLBACK original_source=1");
  }
  const int managed=managed_progress::slot(bookIdentity.c_str());
  if(managed>=0&&bookLength>managed_progress::MAX_BYTES)return false;
  String oldPageKey=bookmarkKey;bookmarkKey=managed>=0?String(managed_progress::key(managed)):contentKey(false);
  reader_work::Budget budget(millis(),reader_work::INDEX_INTERVAL_MS);
  uint32_t offset=0;
  if(!reset){
    if(managed>=0){
      uint8_t digest[32];mbedtls_sha256_ret((const uint8_t*)book,bookLength,digest,0);
      if(!managed_progress::read(prefs,managed,digest,bookLength,offset)){Console.println("PROGRESS_READ_FAILED managed=1");return false;}
      // New Push content has no saved offset. Keep the old page when its
      // context is still present, otherwise start at the first page.
      if(!offset&&livePushAnchor.valid){
        uint32_t moved=0;
        if(bookmarks::relocate(livePushAnchor,book,bookLength,moved,[](){delay(1);})&&moved<bookLength)offset=moved;
        Console.printf("PUSH_RELOCATED found=%u offset=%u\n",unsigned(offset!=0),unsigned(offset));
      }
      livePushAnchor=bookmarks::Anchor();
    }else if(prefs.isKey(bookmarkKey.c_str())){
      offset=prefs.getUInt(bookmarkKey.c_str(),UINT32_MAX);
      if(offset==UINT32_MAX){Console.println("PROGRESS_READ_FAILED");return false;}
    }
    else if(migrateLegacy){
      String oldContentKey=contentKey(true);
      if(prefs.isKey(oldContentKey.c_str())){
        offset=prefs.getUInt(oldContentKey.c_str(),UINT32_MAX);
        if(offset==UINT32_MAX){Console.println("PROGRESS_READ_FAILED");return false;}
      }
      else if(anchor&&anchor->valid){
        if(!bookmarks::relocate(*anchor,book,bookLength,offset,[](){delay(1);})){
          Console.println("PROGRESS_MIGRATION_FAILED context_not_found");return false;
        }
        Console.printf("PROGRESS_MIGRATED old_offset=%u new_offset=%u\n",anchor->offset,offset);
      }else if(prefs.isKey(oldPageKey.c_str())){
        uint32_t oldPage=prefs.getUInt(oldPageKey.c_str(),UINT32_MAX);
        if(oldPage==UINT32_MAX){Console.println("PROGRESS_READ_FAILED");return false;}
        for(uint32_t n=0;n<oldPage&&offset<bookLength;n++){
          size_t next=layoutPage(offset,false,64,&budget);if(next<=offset)break;offset=next;
        }
      }else if(requireRecovery){
        Console.println("PROGRESS_MIGRATION_FAILED cache_unavailable");return false;
      }
    }
  }
  if(offset>=bookLength){
    pageOffsets.clear();Console.println("PROGRESS_READ_FAILED offset_out_of_range");return false;
  }
  if(progressive){
    const uint32_t start=progressive_index::safeStart(book,bookLength,offset);
    if(!progressiveIndex.begin(bookLength,start,pageOffsets,pageNumber,
        [&](size_t at){return layoutPage(at,false,24,&budget);})){pageOffsets.clear();return false;}
    if(progressiveIndex.pageEnd(pageOffsets,pageNumber)<=offset){
      cancelReaderIndex();pageOffsets.clear();Console.println("PROGRESS_READ_FAILED anchor_not_visible");return false;
    }
    indexedBook=book;indexedLength=bookLength;indexedKey=bookmarkKey;indexedStyle=currentIndexStyle();
    indexBudget=reader_work::Budget(millis(),reader_work::INDEX_INTERVAL_MS);
    retainReadingAnchor(offset);
    Console.printf("BOOK_LOADED bytes=%u pages=pending resume_offset=%u window_start=%u\n",
      unsigned(bookLength),offset,start);
    return true;
  }
  if(!pagination::index(bookLength,pageOffsets,[&](size_t p){return layoutPage(p,false,24,&budget);}))return false;
  auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
  pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
  retainReadingAnchor(offset);
  Console.printf("BOOK_LOADED bytes=%u pages=%u resume_offset=%u\n",(unsigned)bookLength,(unsigned)pageOffsets.size(),offset);
  return true;
}
