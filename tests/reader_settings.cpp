#include <cassert>
#include <set>
#include <vector>
#include <Preferences.h>
#include "reader_settings.h"
#include "cache_format.h"
#include "input.h"
#include "shutdown_art.h"
int main(){
 using namespace reader_settings;
 Preferences store;store.blobs["wifiCfgV1"]={1,2,3};store.strings["sdLast"]="keep";
 Config c;assert(!load(store));assert(store.writes==0);
 c.doubleMs=210;c.spacing=2;c.preset=0;c.ink=30;
 store.failWrite=true;assert(!save(store,c)&&current().doubleMs==200);
 store.failWrite=false;assert(save(store,c));current()=Config{};assert(load(store));assert(current().doubleMs==210&&current().spacing==2);
 const auto snapshot=store.blobs;const auto writes=store.writes;assert(save(store,c)&&store.writes==writes);
 assert(store.blobs["wifiCfgV1"]==std::vector<uint8_t>({1,2,3})&&store.strings["sdLast"]=="keep");
 store.blobs["readerCfgV1"][5]^=1;current()=Config{};assert(!load(store)&&current().preset==1);store.blobs=snapshot;
 for(unsigned interval:{100u,200u,210u,400u}){
  current().doubleMs=interval;
  for(unsigned gap:{interval-1,interval,interval+1}){
   controls::Buttons b;b.reset(0,0);b.update(2,1000,true);b.update(0,1010,true);b.update(2,1000+gap,true);
   const auto action=b.update(0,1010+gap,true);assert(action==(gap<=interval?controls::Action::NextScreen:controls::Action::Next));
  }
 }
 std::set<uint32_t> identities;
 for(unsigned p=0;p<3;++p)for(unsigned gap:{0u,2u,4u})for(bool punct:{false,true})for(bool hyphen:{false,true})for(bool justify:{false,true}){
  current()=Config{};current().preset=p;current().spacing=gap;current().punctuation=punct;current().hyphenation=hyphen;current().justify=justify;
  const uint8_t flags=(punct?1:0)|(gap?2:0);const auto id=reader_cache::layoutFor(true,false,flags);
  assert(identities.insert(id).second);assert(reader_cache::matchesLayout(id,false,flags));
  current().doubleMs=333;current().ink=12;current().refresh=0;assert(reader_cache::matchesLayout(id,false,flags));
  current().preset=(p+1)%3;assert(!reader_cache::matchesLayout(id,false,flags));
 }
 current()=Config{};unsigned ink=0;
 for(int y=0;y<4;++y)for(int x=0;x<4;++x){assert(keepInk(x,y)==bool((x+y)%2));ink+=keepInk(x,y);}assert(ink==8);
 current().ink=0;assert(!keepInk(1,0));current().ink=100;assert(keepInk(0,0));
 std::vector<uint8_t> art(18,0);memcpy(art.data(),"PDI1",4);art[4]=9;art[6]=1;art[16]=128;
 uint32_t checksum=crc(art.data()+16,2);for(int i=0;i<4;i++)art[12+i]=checksum>>(8*i);
 assert(shutdown_art::valid(art.data(),art.size()));art[17]^=1;assert(!shutdown_art::valid(art.data(),art.size()));
 current()=Config{};
}
