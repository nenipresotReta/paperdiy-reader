#include "sd_reader.h"
#include "reader.h"
#include <M5Unified.h>
#include <mbedtls/sha256.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include "epub_core.h"
#include "sd_cache.h"
bool sdOK=false;
std::vector<String> sdBooks;
size_t selectedBook=0;
String sdError;
uint32_t crcUpdate(uint32_t c,const uint8_t* p,size_t n){
  while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320:0);}
  return c;
}

void scanDirectory(const String& path,unsigned depth){
  Serial.printf("SD_DIR %s\n",path.c_str());
  File dir=SD.open(path);if(!dir||!dir.isDirectory())return;
  while(sdBooks.size()<256){
    File f=dir.openNextFile();if(!f)break;String name=f.name();
    if(!name.startsWith(".")){
      String full=path=="/"?"/"+name:path+"/"+name;
      String lower=name;lower.toLowerCase();
      if(f.isDirectory()&&lower.endsWith(".epub")){sdBooks.push_back(full);}
      else if(f.isDirectory()&&depth<3){f.close();scanDirectory(full,depth+1);}
      else {String lower=name;lower.toLowerCase();if(!f.isDirectory()&&(lower.endsWith(".txt")||lower.endsWith(".epub")))sdBooks.push_back(full);}
    }
    f.close();delay(1);
  }
}
bool scanSD(){
  String previous=selectedBook<sdBooks.size()?sdBooks[selectedBook]:prefs.getString("sdLast","");
  sdBooks.clear();selectedBook=0;sdError="";
  pinMode(48,INPUT_PULLUP);
  Serial.printf("SD_DETECT level=%d (active low)\n",digitalRead(48));
  SD.end();SPI.begin(39,40,38,47);
  sdOK=false;
  for(uint32_t frequency : {4000000u,1000000u,400000u}){
    SD.end();
    pinMode(47,OUTPUT);digitalWrite(47,HIGH);
    pinMode(40,INPUT_PULLUP);
    delay(100);SPI.begin(39,40,38,47);
    Serial.printf("SD_TRY frequency=%u\n",frequency);
    sdOK=SD.begin(47,SPI,frequency);
    Serial.printf("SD_TRY_DONE ok=%d\n",sdOK);
    if(sdOK&&SD.cardType()!=CARD_NONE)break;
  }
  if(!sdOK||SD.cardType()==CARD_NONE){sdOK=false;sdError="No SD card / mount failed (FAT32)";Serial.println("SD_NOT_MOUNTED");return false;}
  Serial.println("SD_SCAN_BEGIN");scanDirectory("/",0);Serial.println("SD_SCAN_DONE");std::sort(sdBooks.begin(),sdBooks.end(),[](const String& a,const String& b){return a.compareTo(b)<0;});
  for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==previous){selectedBook=i;break;}
  Serial.printf("SD_MOUNTED size_mb=%llu books=%u\n",SD.cardSize()/1024/1024,(unsigned)sdBooks.size());
  for(size_t i=0;i<sdBooks.size();i++)Serial.printf("SD_FILE %u %s\n",(unsigned)i,sdBooks[i].c_str());return true;
}
bool validUtf8(const char* data,size_t size){
  size_t p=0;while(p<size){uint8_t c=data[p++];if(c<128){if(!c)return false;continue;}int n;uint32_t code,min;
    if(c>=0xc2&&c<=0xdf){n=1;code=c&31;min=128;}else if(c>=0xe0&&c<=0xef){n=2;code=c&15;min=2048;}else if(c>=0xf0&&c<=0xf4){n=3;code=c&7;min=65536;}else return false;
    if(size-p<(size_t)n)return false;while(n--){uint8_t b=data[p++];if((b&0xc0)!=0x80)return false;code=(code<<6)|(b&63);}if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
  }return true;
}
bool openSD(size_t index,bool paginate){
  uint32_t started=millis();pageOffsets.clear();bookLength=0;
  sdError="";if(index>=sdBooks.size()||!book){sdError="Book unavailable";return false;}
  File f=SD.open(sdBooks[index]);if(!f){sdError="Cannot read SD file";return false;}
  bool directory=f.isDirectory();f.close();
  BookCache cache(sdBooks[index],directory);
  if(cache.load()){selectedBook=index;prefs.putString("sdLast",sdBooks[index]);Serial.printf("SD_OPENED %s ms=%u cached=1\n",sdBooks[index].c_str(),(unsigned)(millis()-started));return true;}
  cache.dependencies.clear();if(!directory)cache.observe("");
  f=SD.open(sdBooks[index]);if(!f){sdError="Cannot read SD file";return false;}
  Serial.println("CACHE_MISS");
  String lower=sdBooks[index];lower.toLowerCase();bool ok=false;pageOffsets.clear();
  if(lower.endsWith(".txt")){
    bookLength=f.size();if(!bookLength||bookLength>MAX_BOOK){sdError="TXT exceeds 4 MiB / empty";return false;}
    ok=f.read((uint8_t*)book,bookLength)==bookLength;
    if(ok&&bookLength>=3&&!memcmp(book,"\xef\xbb\xbf",3)){memmove(book,book+3,bookLength-3);bookLength-=3;}
    if(ok&&!validUtf8(book,bookLength)){sdError="TXT must be UTF-8; convert encoding";ok=false;}
  }else{
    if(!directory&&f.size()>64*1024*1024){sdError="EPUB exceeds 64 MiB";return false;}
    epub::Importer importer;
    if(directory){
      String base=sdBooks[index];f.close();
      importer.resourceSize=[&cache](const std::string& name)->size_t{return cache.observe(name);};
      importer.resourceRead=[base](const std::string& name,void* data,size_t size)->bool{File r=SD.open(base+"/"+name.c_str());return r&&!r.isDirectory()&&r.read((uint8_t*)data,size)==size;};
    }
    importer.fileSize=directory?0:f.size();importer.output=book;importer.capacity=MAX_BOOK;
    importer.alloc=[](size_t n)->void*{return ps_malloc(n);};importer.tick=[](){delay(1);};
    importer.read=[&f](uint32_t offset,void* p,size_t n)->size_t{if(!f.seek(offset))return 0;return f.read((uint8_t*)p,n);};
    ok=importer.run();if(ok)bookLength=importer.used;else sdError=importer.error.c_str();
  }
  f.close();if(!ok){bookLength=0;Serial.printf("SD_OPEN_FAILED %s\n",sdError.c_str());return false;}
  book[bookLength]=0;
  if(!paginate)return true;
  uint32_t hash=crcUpdate(0xffffffff,(const uint8_t*)sdBooks[index].c_str(),sdBooks[index].length())^0xffffffff;
  char key[12];snprintf(key,sizeof(key),"s%08lx",(unsigned long)hash);bookmarkKey=key;
  bookIdentity=sdBooks[index];
  Serial.printf("BOOK_PARSE_DONE bytes=%u ms=%u free_psram=%u\n",(unsigned)bookLength,(unsigned)(millis()-started),ESP.getFreePsram());
  if(!indexBook()){sdError="Pagination failed";return false;}
  if(!cache.save())Serial.println("CACHE_SAVE_SKIPPED");
  selectedBook=index;prefs.putString("sdLast",sdBooks[index]);Serial.printf("SD_OPENED %s ms=%u\n",sdBooks[index].c_str(),(unsigned)(millis()-started));return true;
}
