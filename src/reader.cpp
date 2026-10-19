#include "reader.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <algorithm>
#include <mbedtls/sha256.h>
#include "reader_font.h"
#include "reading_font.h"
Preferences prefs;
char* book=nullptr;
size_t bookLength=0;
std::vector<uint32_t> pageOffsets;
uint32_t pageNumber=0;
String bookmarkKey="page",bookIdentity="uploaded";
size_t utf8Char(const char* s,size_t remain,char out[5]){
  unsigned char c=*s;size_t n=c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
  if(n>remain)n=1;
  memcpy(out,s,n);out[n]=0;return n;
}
int16_t asciiWidths[128];
void prepareReaderFont(){
  M5.Display.setFont(bodyFont());M5.Display.setTextSize(1);
  for(int i=0;i<128;++i)asciiWidths[i]=bodyGlyphAdvance(i);
}
// Layout and drawing share exactly the same line wrapping and offsets.
size_t layoutPage(size_t from,bool draw,int top){
  const int left=24,right=516,bottom=886,lineHeight=READER_LINE_HEIGHT;
  int x=left,y=top;size_t p=from;uint32_t yielded=millis();
  while(p<bookLength){
    char ch[5];size_t n=utf8Char(book+p,bookLength-p,ch);
    if(ch[0]=='\r'){p+=n;continue;}
    if(ch[0]=='\n'){p+=n;x=left;y+=lineHeight;if(y>bottom)break;continue;}
    if(ch[0]=='\t'){ch[0]=' ';ch[1]=0;}
    int w=(uint8_t)ch[0]<128?asciiWidths[(uint8_t)ch[0]]:bodyGlyphAdvance(readerCodepoint(ch,n));
    if(x+w>right){x=left;y+=lineHeight;if(y>bottom)break;}
    if(draw)drawBodyGlyph(ch,n,x,y);
    x+=w;p+=n;
    if((p&1023)<4&&millis()-yielded>=8){delay(1);yielded=millis();}
  }
  return p;
}
bool indexBook(bool reset,bool migrateLegacy){
  if(!bookLength)return false;book[bookLength]=0;
  M5.Display.setFont(readerFont());M5.Display.setTextSize(1);
  prepareReaderFont();
  String oldKey=bookmarkKey;
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
  mbedtls_sha256_update_ret(&sha,(const uint8_t*)bookIdentity.c_str(),bookIdentity.length());
  for(size_t p=0;p<bookLength;p+=4096){mbedtls_sha256_update_ret(&sha,(const uint8_t*)book+p,std::min(size_t(4096),bookLength-p));delay(1);}
  uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);
  char key[16];key[0]='b';for(int i=0;i<7;i++)snprintf(key+1+i*2,3,"%02x",digest[i]);bookmarkKey=key;
  uint32_t offset=0;
  if(!reset){
    if(prefs.isKey(bookmarkKey.c_str()))offset=prefs.getUInt(bookmarkKey.c_str(),0);
    else if(migrateLegacy){uint32_t oldPage=prefs.getUInt(oldKey.c_str(),0);for(uint32_t n=0;n<oldPage&&offset<bookLength;n++){size_t next=layoutPage(offset,false,64);if(next<=offset)break;offset=next;}}
  }
  pageOffsets.clear();size_t pos=0;
  while(pos<bookLength){pageOffsets.push_back(pos);size_t next=layoutPage(pos,false);if(next<=pos)return false;pos=next;}
  if(offset>=bookLength)offset=0;
  auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
  Serial.printf("BOOK_LOADED bytes=%u pages=%u\n",(unsigned)bookLength,(unsigned)pageOffsets.size());return true;
}
