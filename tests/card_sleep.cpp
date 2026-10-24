#include "card_sleep.h"
#include "storage_validation.h"
#include <cassert>
#include <string>

struct Pins {
  enum Trigger {Edge,High,Low};
  Trigger trigger=Edge;
  bool interrupt=true,wake=false;
  int pin=0;
  char fail=0;
  std::string calls;
  bool disableInterrupt(){calls+='d';if(fail=='d')return false;interrupt=false;return true;}
  bool armWake(int initial){calls+='a';assert(!interrupt);trigger=initial?Low:High;wake=true;return fail!='a';}
  bool disarmWake(){calls+='w';if(fail=='w')return false;wake=false;return true;}
  bool restoreEdge(){calls+='e';if(fail=='e')return false;trigger=Edge;return true;}
  bool enableInterrupt(){calls+='i';assert(trigger==Edge);if(fail=='i')return false;interrupt=true;return true;}
  int level(){return pin;}
  void revokeProofs(){calls+='p';storage_validation::sourceChanged();}
  void revokeTrust(){calls+='r';storage_validation::invalidate();}
  void signalChange(){calls+='s';storage_validation::signalCardEvent();}
};

int main(){
  // Normal sleep, a cancelled attempt and sleep/configuration failures share
  // this adapter. Every path restores ANYEDGE before enabling the card ISR.
  for(int initial:{0,1})for(int outcome=0;outcome<3;++outcome){
    storage_validation::beginExclusive();const auto old=storage_validation::lease();
    uint8_t key[32]={},digest[32]={},out[32];
    storage_validation::remember(old,key,12,34,digest);storage_validation::rememberGraph(old,key,digest);
    Pins pins;pins.pin=initial;
    {
      card_sleep::Window<Pins> window(pins,initial);assert(window.begin());
      assert(!pins.interrupt&&pins.wake&&pins.trigger==(initial?Pins::Low:Pins::High));
      assert(storage_validation::lease()!=old&&!storage_validation::find(key,12,34,out));
      assert(!storage_validation::verifiedGraph(key,digest));
      // 0 normal timer wake, 1 cancellation, 2 sleep API failure. Destructor
      // fallback is intentional for the latter to guard future early returns.
      if(outcome!=2){assert(window.finish());assert(window.finish());}
    }
    assert(pins.interrupt&&!pins.wake&&pins.trigger==Pins::Edge&&pins.calls=="pdawei");
    assert(!storage_validation::cardEventPending()); // Never fake an insertion.
  }
  for(char failure:{'d','a','w','e','i'}){
    storage_validation::beginExclusive();Pins pins;pins.fail=failure;
    {
      card_sleep::Window<Pins> window(pins,0);
      const bool began=window.begin();assert(began==(failure!='d'&&failure!='a'));
      const bool restored=window.finish();assert(restored==(failure=='d'||failure=='a'));
      if(!restored)assert(!storage_validation::lease());
    }
    if(failure=='e')assert(!pins.interrupt&&pins.calls.find('i')==std::string::npos);
    assert(pins.calls.find('w')!=std::string::npos&&pins.calls.find('e')!=std::string::npos);
  }
  for(int initial:{0,1}){
    storage_validation::beginExclusive();Pins pins;pins.pin=initial;
    card_sleep::Window<Pins> window(pins,initial);assert(window.begin());pins.pin=!initial;
    assert(window.finish()&&storage_validation::cardEventPending()&&!storage_validation::lease());
    assert(pins.calls=="pdaweis");
  }
  storage_validation::beginExclusive();Pins untouched;
  {card_sleep::Window<Pins> window(untouched,0);}
  assert(untouched.calls.empty()&&untouched.interrupt);
}
