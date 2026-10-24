#pragma once
#include <cstdint>
namespace sd_speed {
// PaperDIY's factory SDSPI driver targets 20 MHz. A card/bus failure lowers
// the ceiling for the next mount; a newly inserted card starts a fresh trial.
constexpr uint32_t FREQUENCIES[]={20000000u,10000000u,4000000u,1000000u,400000u};
struct Policy {
  uint32_t ceiling=FREQUENCIES[0],active=0;
  bool accepts(uint32_t frequency)const{return frequency<=ceiling;}
  void mounted(uint32_t frequency){active=frequency;}
  void readFailed(){
    if(active){for(unsigned i=0;i<4;++i)if(active==FREQUENCIES[i]){ceiling=FREQUENCIES[i+1];break;}}
    active=0;
  }
  void newCard(){ceiling=FREQUENCIES[0];active=0;}
};
}
