#include "resume_store.h"
#include "library_rows.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
struct Preferences {
  std::map<std::string,std::vector<uint8_t>> blobs;
  std::map<std::string,uint32_t> values;
  std::map<std::string,std::string> strings;
  bool failWrite=false,failRead=false;
  unsigned writes=0;
  bool isKey(const char* k){return blobs.count(k)||strings.count(k);}
  size_t getBytesLength(const char* k){return blobs.count(k)?blobs[k].size():0;}
  size_t getBytes(const char* k,void* p,size_t n){
    if(failRead||!blobs.count(k)||n<blobs[k].size())return 0;
    std::memcpy(p,blobs[k].data(),blobs[k].size());return blobs[k].size();
  }
  size_t putBytes(const char* k,const void* p,size_t n){
    ++writes;if(failWrite)return 0;
    auto b=static_cast<const uint8_t*>(p);blobs[k]={b,b+n};return n;
  }
  uint32_t getUInt(const char* key,uint32_t fallback){
    auto found=values.find(key);return failRead||found==values.end()?fallback:found->second;
  }
  size_t putUInt(const char* key,uint32_t n){++writes;if(failWrite)return 0;values[key]=n;return 4;}
  std::string getString(const char* key,const char* fallback){
    auto found=strings.find(key);return failRead||found==strings.end()?fallback:found->second;
  }
  size_t putString(const char* key,const char* value){
    ++writes;if(failWrite)return 0;strings[key]=value;return strings[key].size();
  }
};
int main(){
  using namespace resume_state;
  Preferences prefs; Record saved,loaded;
  assert(read(prefs,loaded)==ReadResult::Absent);
  saved.screen=1;std::strcpy(saved.path,"/Books/数学.md");
  assert(save(prefs,saved)&&read(prefs,loaded)==ReadResult::Valid);
  assert(loaded.screen==1&&!std::strcmp(loaded.path,saved.path));
  auto before=prefs.blobs;unsigned writes=prefs.writes;
  assert(save(prefs,saved)&&prefs.writes==writes);
  saved.screen=2;saved.settingsId=SettingsId::Network;
  prefs.failWrite=true;assert(!save(prefs,saved)&&prefs.blobs==before);
  prefs.failWrite=false;prefs.failRead=true;assert(!save(prefs,saved));
  prefs.failRead=false;assert(save(prefs,saved));
  std::vector<std::string> books={"/A.txt","/Books/数学.md","/Z.epub"};
  assert(selectedIndex(saved,books)==1);
  books.insert(books.begin(),"/0.txt");assert(selectedIndex(saved,books)==2);
  books.erase(books.begin()+2);assert(selectedIndex(saved,books)==0);
  for(size_t n=0;n<HEADER;++n){prefs.blobs[KEY].resize(n);assert(read(prefs,loaded)==ReadResult::Invalid);}
  prefs.blobs.clear();assert(save(prefs,saved));
  prefs.blobs[KEY][6]^=1;assert(read(prefs,loaded)==ReadResult::Invalid);
  prefs.blobs.clear();assert(save(prefs,saved));prefs.blobs[KEY][4]=3;
  assert(read(prefs,loaded)==ReadResult::Invalid);
  prefs.failWrite=true;assert(!saveRotation(prefs,true)&&prefs.getUInt("rot180",0)==0);
  prefs.failWrite=false;assert(saveRotation(prefs,true)&&prefs.getUInt("rot180",0)==1);
  writes=prefs.writes;assert(saveRotation(prefs,true)&&prefs.writes==writes);
  prefs.failRead=true;assert(!saveRotation(prefs,false));
  prefs.failRead=false;assert(!liveSelected(prefs));
  prefs.failWrite=true;assert(!saveLiveSelected(prefs,true)&&!liveSelected(prefs));
  prefs.failWrite=false;assert(saveLiveSelected(prefs,true)&&liveSelected(prefs));
  writes=prefs.writes;assert(saveLiveSelected(prefs,true)&&prefs.writes==writes);
  assert(saveLiveSelected(prefs,false)&&!liveSelected(prefs));
  prefs.values["liveP"]=7;assert(!liveSelected(prefs)); // only an exact 1 restores P
  prefs.failRead=true;assert(!saveLiveSelected(prefs,true));
  Record tooLong;std::memset(tooLong.path,'a',sizeof(tooLong.path));
  assert(!save(prefs,tooLong));
  prefs.failRead=false;
  // Old binary records map boolean settings to the original stable ID. Reading
  // never migrates/writes either snapshot or the unrelated progress/Wi-Fi keys.
  prefs.blobs.clear();prefs.values["book-progress"]=123;prefs.values["wifi-marker"]=456;
  const std::vector<uint8_t> legacy={'P','R','S',1,2,1,0,0};
  prefs.blobs[LEGACY_KEY]=legacy;writes=prefs.writes;
  assert(read(prefs,loaded)==ReadResult::Valid);
  assert(loaded.settingsId==SettingsId::Network && loaded.screen==2);
  assert(prefs.writes==writes&&!prefs.blobs.count(KEY));
  assert(save(prefs,saved)&&prefs.blobs[LEGACY_KEY]==legacy);
  assert(read(prefs,loaded)==ReadResult::Valid&&loaded.settingsId==SettingsId::Network);
  // The retired V2 page maps to Network, including its scene and selected book
  // path. The wire value stays untouched until a normal future shutdown.
  prefs.blobs[KEY][5]=settings_registry::RETIRED_TYPOGRAPHY_ID;
  const auto retired=prefs.blobs;writes=prefs.writes;
  assert(read(prefs,loaded)==ReadResult::Valid&&loaded.settingsId==SettingsId::Network);
  assert(loaded.screen==2&&!std::strcmp(loaded.path,saved.path));
  assert(prefs.blobs==retired&&prefs.writes==writes);
  prefs.blobs[KEY][5]=255;
  assert(read(prefs,loaded)==ReadResult::Invalid); // no resurrection of V1
  prefs.blobs.erase(KEY);prefs.blobs[LEGACY_KEY][5]=0;
  assert(read(prefs,loaded)==ReadResult::Valid&&loaded.settingsId==SettingsId::None);
  prefs.blobs[LEGACY_KEY][3]=2;
  assert(read(prefs,loaded)==ReadResult::Invalid); // key and wire version must agree
  prefs.blobs[LEGACY_KEY][3]=1;prefs.blobs[LEGACY_KEY][5]=2;
  assert(read(prefs,loaded)==ReadResult::Invalid); // V1 never knew typography
  prefs.blobs.clear();saved.settingsId=static_cast<SettingsId>(255);
  assert(!save(prefs,saved));
  assert(settings_registry::COUNT==1);
  assert(settings_registry::ENTRIES[0].id==SettingsId::Network);
  assert(!settings_registry::valid(static_cast<SettingsId>(2)));
  assert(prefs.values["book-progress"]==123&&prefs.values["wifi-marker"]==456);

  // V3 is V2 with the cursor on Recent. Only that snapshot uses it; every
  // other snapshot keeps the V2 wire format.
  prefs.blobs.clear();saved=Record{};std::strcpy(saved.path,"/Books/数学.md");
  assert(save(prefs,saved)&&prefs.blobs[KEY][3]==2);
  saved.recent=true;assert(save(prefs,saved)&&prefs.blobs[KEY][3]==3);
  assert(read(prefs,loaded)==ReadResult::Valid&&loaded.recent);
  assert(loaded.screen==0&&loaded.settingsId==SettingsId::None&&!std::strcmp(loaded.path,saved.path));
  saved.screen=1;assert(save(prefs,saved)&&read(prefs,loaded)==ReadResult::Valid);
  assert(loaded.recent&&loaded.screen==1);
  saved.recent=false;assert(save(prefs,saved)&&prefs.blobs[KEY][3]==2);
  assert(read(prefs,loaded)==ReadResult::Valid&&!loaded.recent);
  before=prefs.blobs;
  saved.recent=true;saved.settingsId=SettingsId::Network;
  assert(!save(prefs,saved)&&prefs.blobs==before); // Recent is never a settings book
  saved.settingsId=SettingsId::None;saved.path[0]=0;
  assert(!save(prefs,saved)&&prefs.blobs==before); // Recent always names a book
  std::strcpy(saved.path,"/Books/数学.md");assert(save(prefs,saved));
  prefs.blobs[KEY][5]=1;assert(read(prefs,loaded)==ReadResult::Invalid);
  prefs.blobs[KEY]={'P','R','S',3,0,0,0,0};assert(read(prefs,loaded)==ReadResult::Invalid);
  prefs.blobs[KEY]={'P','R','S',4,0,0,0,0};assert(read(prefs,loaded)==ReadResult::Invalid);
  prefs.blobs.erase(KEY);prefs.blobs[LEGACY_KEY]={'P','R','S',3,0,0,1,0,'/'};
  assert(read(prefs,loaded)==ReadResult::Invalid); // V1 key never holds V3

  // Library rows: settings, Recent while it names a listed book, then books.
  using namespace library_rows;
  assert(settings_registry::COUNT==1);
  Layout hidden{25,25};
  assert(!hidden.hasRecent()&&hidden.count()==26);
  assert(cursor(hidden,{Kind::Settings,0})==0&&cursor(hidden,{Kind::Book,20})==21);
  assert(cursor(hidden,{Kind::Recent,4})==5); // a hidden Recent is its list row
  assert(at(hidden,0).kind==Kind::Settings&&at(hidden,1).kind==Kind::Book&&at(hidden,1).index==0);
  Layout shown{25,4};
  assert(shown.hasRecent()&&shown.count()==27);
  assert(cursor(shown,{Kind::Recent,4})==1&&cursor(shown,{Kind::Book,0})==2);
  assert(cursor(shown,{Kind::Book,4})==6&&cursor(shown,{Kind::Book,24})==26);
  assert(at(shown,1).kind==Kind::Recent&&at(shown,1).index==4);
  assert(at(shown,2).kind==Kind::Book&&at(shown,2).index==0);
  assert(at(shown,26).kind==Kind::Book&&at(shown,26).index==24);
  for(size_t c=0;c<shown.count();++c)assert(cursor(shown,at(shown,c))==c);
  for(size_t c=0;c<hidden.count();++c)assert(cursor(hidden,at(hidden,c))==c);
  Layout empty{0,0};assert(!empty.hasRecent()&&empty.count()==1);

  // Recent is stored by path, and rewritten only when the book changes.
  prefs=Preferences{};
  std::vector<std::string> listed={"/A.txt","/Books/数学.md","/Z.epub"};
  assert(recent_book::index("",listed)==3&&recent_book::index("/gone.txt",listed)==3);
  assert(recent_book::index("/Z.epub",listed)==2);
  assert(!recent_book::save(prefs,"")&&!recent_book::save(prefs,"A.txt")&&prefs.writes==0);
  assert(recent_book::save(prefs,"/Books/数学.md")&&prefs.strings[recent_book::KEY]=="/Books/数学.md");
  writes=prefs.writes;assert(recent_book::save(prefs,"/Books/数学.md")&&prefs.writes==writes);
  prefs.failWrite=true;assert(!recent_book::save(prefs,"/Z.epub"));
  assert(prefs.strings[recent_book::KEY]=="/Books/数学.md");
  prefs.failWrite=false;prefs.failRead=true;assert(!recent_book::save(prefs,"/Z.epub"));
  prefs.failRead=false;assert(recent_book::save(prefs,"/Z.epub")&&recent_book::index(prefs.strings[recent_book::KEY].c_str(),listed)==2);
  puts("Atomic V1/V2/V3 resume, retired page migration, stable settings IDs and library rows: PASS");
}
