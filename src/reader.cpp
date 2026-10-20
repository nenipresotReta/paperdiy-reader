#include "reader.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <mbedtls/sha256.h>
#include "reader_font.h"
#include "reading_font.h"
#include "bookmark_store.h"
#include "pagination.h"
Preferences prefs;
char* book=nullptr;
size_t bookLength=0;
std::vector<uint32_t> pageOffsets;
uint32_t pageNumber=0;
String bookmarkKey="page",bookIdentity="uploaded";
bool progressSaveFailed=false;
namespace {
bool progressAvailable=false;
String contentKey(bool legacy){
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
  bookmarks::identity([&](const uint8_t* bytes,size_t count){
    mbedtls_sha256_update_ret(&sha,bytes,count);delay(1);
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
bool saveReadingProgress(){
  if(pageOffsets.empty()||pageNumber>=pageOffsets.size())return false;
  auto result=bookmarks::save(prefs,progressAvailable,bookmarkKey.c_str(),
    pageOffsets[pageNumber],bookIdentity.c_str(),bookIdentity.length());
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
  pagination::Geometry g;g.top=top;g.lineHeight=READER_LINE_HEIGHT;
  uint32_t yielded=millis();
  return pagination::page(book,bookLength,from,g,
    [](const char* ch,size_t n){return (uint8_t)ch[0]<128?asciiWidths[(uint8_t)ch[0]]:bodyGlyphAdvance(readerCodepoint(ch,n));},
    [draw](const char* ch,size_t n,int x,int y){if(draw)drawBodyGlyph(ch,n,x,y);},
    [&](){if(millis()-yielded>=8){delay(1);yielded=millis();}});
}
bool indexBook(bool reset,bool migrateLegacy,const bookmarks::Anchor* anchor,bool requireRecovery){
  if(!bookLength)return false;book[bookLength]=0;prepareReaderFont();
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
  if(!pagination::index(bookLength,pageOffsets,[](size_t p){return layoutPage(p,false);}))return false;
  if(offset>=bookLength)offset=bookLength-1;
  auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
  pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
  Serial.printf("BOOK_LOADED bytes=%u pages=%u resume_offset=%u\n",(unsigned)bookLength,(unsigned)pageOffsets.size(),offset);
  return true;
}
