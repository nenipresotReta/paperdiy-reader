#include <cassert>
#include <set>
#include <vector>
#include <Preferences.h>
#include "reader_settings.h"
#include "input.h"
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
 current()=Config{};unsigned ink=0;
 for(int y=0;y<4;++y)for(int x=0;x<4;++x){assert(keepInk(x,y)==bool((x+y)%2));ink+=keepInk(x,y);}assert(ink==8);
 current().ink=0;assert(!keepInk(1,0));current().ink=100;assert(keepInk(0,0));
 current()=Config{};
}
