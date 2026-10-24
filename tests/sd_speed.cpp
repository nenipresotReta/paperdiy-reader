#include "sd_speed.h"
#include <cassert>
int main(){
 sd_speed::Policy p;
 assert(p.accepts(20000000));
 for(unsigned i=0;i<5;++i){
  const auto speed=sd_speed::FREQUENCIES[i];assert(p.accepts(speed));
  p.mounted(speed);p.readFailed();assert(p.active==0);
  if(i<4)assert(!p.accepts(speed)&&p.accepts(sd_speed::FREQUENCIES[i+1]));
 }
 assert(p.accepts(400000)&&!p.accepts(1000000));
 p.newCard();assert(p.accepts(20000000));p.readFailed();assert(p.accepts(20000000));
 // A slower successful mount determines the next fallback, not the original ceiling.
 p.mounted(4000000);p.readFailed();assert(p.ceiling==1000000);
}
