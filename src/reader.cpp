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
#include "reader_typography.h"
namespace reader_typography {const Style style;}
Preferences prefs;
char* book=nullptr;
size_t bookLength=0;
std::vector<uint32_t> pageOffsets;
uint32_t pageNumber=0;
String bookmarkKey="page",bookIdentity="uploaded";
bool progressSaveFailed=false;
bool bookEnglishHyphens=true;
bool bookMarkdown=false,bookMarkupFallback=false;
namespace {
bool progressAvailable=false;
uint32_t retainedOffset=0,retainedPage=UINT32_MAX;
char retainedKey[16]={};
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
  if(!progressAvailable)Serial.println("PROGRESS_FAILED operation=begin");
  return progressAvailable;
}
void retainReadingAnchor(uint32_t offset){
  retainedOffset=offset;retainedPage=pageNumber;
  strncpy(retainedKey,bookmarkKey.c_str(),sizeof(retainedKey)-1);
  retainedKey[sizeof(retainedKey)-1]=0;
}
uint32_t currentReadingOffset(){
  if(pageOffsets.empty()||pageNumber>=pageOffsets.size())return UINT32_MAX;
  const uint32_t start=pageOffsets[pageNumber];
  const size_t end=pageNumber+1<pageOffsets.size()?pageOffsets[pageNumber+1]:bookLength;
  // Reflow chooses a containing page, not a new reading position. Keep the
  // precise source anchor until navigation actually changes the page.
  if(retainedPage==pageNumber&&bookmarkKey==retainedKey&&
     retainedOffset>=start&&retainedOffset<end)return retainedOffset;
  return start;
}
bool saveReadingProgress(){
  const uint32_t offset=currentReadingOffset();
  if(offset==UINT32_MAX)return false;
  // This is the current in-memory location even if persistence must be retried.
  retainReadingAnchor(offset);
  auto result=bookmarks::save(prefs,progressAvailable,bookmarkKey.c_str(),
    offset,bookIdentity.c_str(),bookIdentity.length());
  progressSaveFailed=result!=bookmarks::SaveResult::Saved;
  if(progressSaveFailed)Serial.printf("PROGRESS_FAILED operation=%u\n",unsigned(result));
  return !progressSaveFailed;
}
size_t utf8Char(const char* s,size_t remain,char out[5]){
  size_t n=pagination::utf8Bytes(s,remain);memcpy(out,s,n);out[n]=0;return n;
}
int16_t asciiWidths[128];
void prepareReaderFont(){
  M5.Display.setFont(bodyFont());M5.Display.setTextSize(1);
  for(int i=0;i<128;++i)asciiWidths[i]=bodyGlyphAdvance(i);
}
// Drawing and cache indexing execute the same layout implementation.
size_t layoutPage(size_t from,bool draw,int top){
  if(bookMarkdown)return layoutMarkdown(from,draw,top);
  // The 30 px right margin is also the display-only hang budget; a full-width
  // closing mark at 510 still ends at or before 540.
  pagination::Geometry g;g.left=24;g.right=510;g.hangLimit=30;
  g.top=top;g.lineHeight=READER_LINE_HEIGHT;g.hyphenate=bookEnglishHyphens;
  uint32_t yielded=millis();
  return reader_typography::page(book,bookLength,from,g,reader_typography::style,
    [](uint32_t cp){
      reader_typography::Metrics result;result.advance=cp<128?asciiWidths[cp]:bodyGlyphAdvance(cp);
      if(typography_experiment::punctuation(cp)!=typography_experiment::Punctuation::None)if(const auto* glyph=bodyGlyph(cp)){
        result.ink=glyph->width&&glyph->height;result.inkLeft=glyph->left;result.inkRight=glyph->left+glyph->width;
      }
      return result;
    },
    [draw](const char* ch,size_t n,int x,int y){if(draw)drawBodyGlyph(ch,n,x,y);},
    [&](){if(millis()-yielded>=8){delay(1);yielded=millis();}});
}
bool indexBook(bool reset,bool migrateLegacy,const bookmarks::Anchor* anchor,bool requireRecovery){
  if(!bookLength)return false;book[bookLength]=0;prepareReaderFont();
  if(bookMarkdown&&!prepareMarkdown(book,bookLength)){
    bookMarkdown=false;bookMarkupFallback=true;
    Serial.println("MARKDOWN_FALLBACK original_source=1");
  }
  String oldPageKey=bookmarkKey;bookmarkKey=contentKey(false);
  uint32_t offset=0;
  if(!reset){
    if(prefs.isKey(bookmarkKey.c_str())){
      offset=prefs.getUInt(bookmarkKey.c_str(),UINT32_MAX);
      if(offset==UINT32_MAX){Serial.println("PROGRESS_READ_FAILED");return false;}
    }
    else if(migrateLegacy){
      String oldContentKey=contentKey(true);
      if(prefs.isKey(oldContentKey.c_str())){
        offset=prefs.getUInt(oldContentKey.c_str(),UINT32_MAX);
        if(offset==UINT32_MAX){Serial.println("PROGRESS_READ_FAILED");return false;}
      }
      else if(anchor&&anchor->valid){
        if(!bookmarks::relocate(*anchor,book,bookLength,offset,[](){delay(1);})){
          Serial.println("PROGRESS_MIGRATION_FAILED context_not_found");return false;
        }
        Serial.printf("PROGRESS_MIGRATED old_offset=%u new_offset=%u\n",anchor->offset,offset);
      }else if(prefs.isKey(oldPageKey.c_str())){
        uint32_t oldPage=prefs.getUInt(oldPageKey.c_str(),UINT32_MAX);
        if(oldPage==UINT32_MAX){Serial.println("PROGRESS_READ_FAILED");return false;}
        for(uint32_t n=0;n<oldPage&&offset<bookLength;n++){
          size_t next=layoutPage(offset,false,64);if(next<=offset)break;offset=next;
        }
      }else if(requireRecovery){
        Serial.println("PROGRESS_MIGRATION_FAILED cache_unavailable");return false;
      }
    }
  }
  if(offset>=bookLength){
    pageOffsets.clear();Serial.println("PROGRESS_READ_FAILED offset_out_of_range");return false;
  }
  if(!pagination::index(bookLength,pageOffsets,[](size_t p){return layoutPage(p,false);}))return false;
  auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
  pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
  retainReadingAnchor(offset);
  Serial.printf("BOOK_LOADED bytes=%u pages=%u resume_offset=%u\n",(unsigned)bookLength,(unsigned)pageOffsets.size(),offset);
  return true;
}
