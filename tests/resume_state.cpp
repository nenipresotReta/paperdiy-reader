#include "resume_store.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
struct Preferences {
  std::map<std::string,std::vector<uint8_t>> blobs;
  std::map<std::string,uint32_t> values;
  bool failWrite=false,failRead=false;
  unsigned writes=0;
  bool isKey(const char* k){return blobs.count(k);}
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
  assert(settings_registry::libraryCursor(99,true,SettingsId::Network)==0);
  assert(settings_registry::libraryCursor(20,false,SettingsId::None)==21);
  assert(!settings_registry::valid(static_cast<SettingsId>(2)));
  assert(prefs.values["book-progress"]==123&&prefs.values["wifi-marker"]==456);
  puts("Atomic V1/V2 resume, retired page migration and stable settings IDs: PASS");
}
