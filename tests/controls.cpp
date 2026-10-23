#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "input.h"
#include "refresh_policy.h"
using controls::Action;
int main(){
  controls::Buttons b;

  for(bool rotated:{false,true})for(bool settings:{false,true}){
    controls::Buttons mapped;
    for(uint8_t physical:{uint8_t(1),uint8_t(2)}){
      mapped.reset(0,0);
      assert(mapped.update(controls::navigationMask(physical,rotated,settings),10,false,false)==Action::None);
      Action expected=physical==1?Action::Previous:Action::Next;
      if(rotated&&!settings)expected=physical==1?Action::Next:Action::Previous;
      assert(mapped.update(0,100,false,false)==expected);
    }
    assert(controls::navigationMask(3,rotated,settings)==3);
  }

  // Single click in the library is deferred to distinguish a double click.
  assert(b.update(1,0,true)==Action::None);
  assert(b.update(0,100,true)==Action::None);
  assert(b.update(0,300,true)==Action::None);
  assert(b.update(0,301,true)==Action::Previous);
  assert(b.update(2,500,true)==Action::None);
  assert(b.update(0,600,true)==Action::None);
  assert(b.update(0,801,true)==Action::Next);

  // Double click on the same key: one page, no single selection.
  b.reset(0,1000);
  assert(b.update(1,1000,true)==Action::None);
  assert(b.update(0,1100,true)==Action::None);
  assert(b.update(1,1200,true)==Action::None);
  assert(b.update(0,1270,true)==Action::PreviousScreen);
  b.reset(0,1400);
  assert(b.update(2,1400,true)==Action::None);
  assert(b.update(0,1500,true)==Action::None);
  assert(b.update(2,1600,true)==Action::None);
  assert(b.update(0,1670,true)==Action::NextScreen);

  // The maximum release-to-release window is 200 ms, inclusive. Merely
  // starting the second press before the deadline must not extend the window.
  for(uint8_t key:{uint8_t(1),uint8_t(2)})for(uint32_t interval:{199u,200u,201u}){
    const Action single=key==1?Action::Previous:Action::Next;
    const Action whole=key==1?Action::PreviousScreen:Action::NextScreen;
    b.reset(0,1000);
    assert(b.update(key,1000,true)==Action::None);
    assert(b.update(0,1010,true)==Action::None);
    assert(b.update(key,1000+interval,true)==Action::None);
    if(interval<=200){
      assert(b.update(0,1010+interval,true)==whole);
      assert(b.idle());
      assert(b.update(0,1500,true)==Action::None);
    }else{
      assert(b.update(0,1010+interval,true)==single);
      // One update returns one action. The following sample observes the
      // second release, which becomes a separate pending single click.
      assert(b.update(0,1011+interval,true)==Action::None);
      assert(b.update(0,1211+interval,true)==Action::None);
      assert(b.update(0,1212+interval,true)==single);
      assert(b.idle());
      assert(b.update(0,1600,true)==Action::None);
    }
  }

  // Consecutive pairs each turn one screen; no delayed single leaks between
  // pairs and no new cooldown is imposed on the next pair.
  for(uint8_t key:{uint8_t(1),uint8_t(2)}){
    b.reset(0,0);
    const Action whole=key==1?Action::PreviousScreen:Action::NextScreen;
    for(uint32_t pair=0;pair<4;++pair){
      const uint32_t start=pair*200;
      assert(b.update(key,start,true)==Action::None);
      assert(b.update(0,start+10,true)==Action::None);
      assert(b.update(key,start+100,true)==Action::None);
      assert(b.update(0,start+110,true)==whole);
      assert(b.idle());
    }
    assert(b.update(0,1200,true)==Action::None);
  }

  // Distinct single clicks expire independently when their gap exceeds 200 ms.
  b.reset(0,0);
  assert(b.update(2,0,true)==Action::None);
  assert(b.update(0,10,true)==Action::None);
  assert(b.update(0,211,true)==Action::Next);
  assert(b.update(2,220,true)==Action::None);
  assert(b.update(0,230,true)==Action::None);
  assert(b.update(0,431,true)==Action::Next);
  assert(b.update(0,800,true)==Action::None);

  // The same inclusive limit holds across millis() rollover.
  const uint32_t wrap=UINT32_MAX-100;
  b.reset(0,wrap);
  assert(b.update(1,wrap,true)==Action::None);
  assert(b.update(0,uint32_t(wrap+10),true)==Action::None);
  assert(b.update(1,uint32_t(wrap+200),true)==Action::None);
  assert(b.update(0,uint32_t(wrap+210),true)==Action::PreviousScreen);

  // A different key flushes the pending single instead of forming a double.
  b.reset(0,1800);
  assert(b.update(1,1800,true)==Action::None);
  assert(b.update(0,1900,true)==Action::None);
  assert(b.update(2,2000,true)==Action::Previous);
  assert(b.update(2,2010,true)==Action::None);
  assert(b.update(0,2100,true)==Action::None);
  assert(b.update(0,2401,true)==Action::Next);

  // Long hold in the library keeps repeating selection and never pages.
  b.reset(0,3000);
  assert(b.update(2,3000,true)==Action::None);
  assert(b.update(2,3599,true)==Action::None);
  assert(b.update(2,3600,true)==Action::Next);
  assert(b.update(2,3950,true)==Action::Next);
  assert(b.update(2,5000,true)==Action::Next);
  assert(b.update(0,5050,true)==Action::None);

  // Reading clicks stay immediate; holds still repeat page turns.
  b.reset(0,6000);
  assert(b.update(2,6000,false)==Action::None);
  assert(b.update(0,6100,false)==Action::Next);
  b.reset(0,6200);
  assert(b.update(1,6200,false)==Action::None);
  assert(b.update(1,6800,false)==Action::Previous);
  assert(b.update(0,6900,false)==Action::None);

  // Network & Rotation: allowRepeat=false means one mode step on release, no hold repeat.
  b.reset(0,7000);
  assert(b.update(1,7000,false,false)==Action::None);
  assert(b.update(1,8000,false,false)==Action::None);
  assert(b.update(0,8100,false,false)==Action::Previous);

  // A+B after a pending click flushes the single first, then confirms.
  b.reset(0,9000);
  assert(b.update(1,9000,true)==Action::None);
  assert(b.update(0,9100,true)==Action::None);
  assert(b.update(3,9200,true)==Action::Previous);
  assert(b.update(3,9300,true)==Action::None);
  assert(b.update(0,9400,true)==Action::Confirm);

  // Reset suppresses until a full release; the chord is not started early.
  b.reset(3,6000);
  assert(b.update(3,6100,true)==Action::None);
  assert(b.update(1,6200,true)==Action::None);
  assert(b.update(0,6300,true)==Action::None);

  // A+B hold still shuts down after two seconds.
  b.reset(0,10000);
  assert(b.update(3,10000,false)==Action::None);
  assert(b.update(3,11999,false)==Action::None);
  assert(b.update(3,12000,false)==Action::Shutdown);
  assert(b.update(3,14000,false)==Action::None);
  assert(b.update(0,14010,false)==Action::None);
  assert(b.update(0,14210,false)==Action::None);
  assert(b.update(0,14220,false)==Action::None);

  // Releasing one key interrupts the continuous shutdown hold.
  b.reset(0,15000);
  b.update(3,15000,true);
  assert(b.update(1,16999,true)==Action::None);
  assert(b.update(3,17000,true)==Action::None);
  assert(b.update(3,18999,true)==Action::None);
  assert(b.update(0,19000,true)==Action::Confirm);

  // Busy completion discards held keys and rapid taps; a fresh press is
  // required after the 200 ms quiet interval, and a library tap is deferred.
  b.discardUntilReleased(20000);
  for(auto mask:{3,2,0,1,0,3,0})assert(b.update(mask,20010,true)==Action::None);
  assert(b.update(0,20199,true)==Action::None);
  assert(b.update(0,20200,true)==Action::None);
  assert(b.update(2,20210,true)==Action::None);
  assert(b.update(0,20300,true)==Action::None);
  assert(b.update(0,20601,true)==Action::None);
  assert(b.update(2,20610,true)==Action::None);
  assert(b.update(0,20710,true)==Action::None);
  assert(b.update(0,21011,true)==Action::Next);

  // The deferred pending click survives the 32-bit millis() wrap.
  b.reset(0,UINT32_MAX-300);
  assert(b.update(1,UINT32_MAX-300,true)==Action::None);
  assert(b.update(1,400,true)==Action::Previous);
  assert(b.update(0,500,true)==Action::None);
  b.reset(0,UINT32_MAX-100);
  assert(b.update(0,UINT32_MAX-100,false)==Action::None);
  assert(b.update(0,100,false)==Action::None);
  assert(b.update(3,110,false)==Action::None);
  assert(b.update(0,120,false)==Action::Confirm);
  assert(b.update(0,130,false)==Action::None);

  // Reset with a held chord: suppressed until all keys are released.
  b.reset(3,16000);
  assert(b.update(3,16100,true)==Action::None);
  assert(b.update(1,16200,true)==Action::None);
  assert(b.update(0,16300,true)==Action::None);
  assert(b.update(1,16400,true)==Action::None);

  // Navigation math and refresh policy.
  assert(controls::moveSelection(0,0,1,true)==0);
  assert(controls::moveSelection(0,27,-1,false)==26);assert(controls::moveSelection(26,27,1,false)==0);
  assert(controls::moveSelection(0,27,1,true)==22);assert(controls::moveSelection(22,27,1,true)==0);
  assert(controls::moveSelection(21,27,1,true)==26);assert(controls::moveSelection(26,27,-1,true)==4);
  RefreshPolicy p;assert(p.choose(true,true));for(int i=0;i<10;++i)assert(!p.choose(false,true));assert(p.choose(false,true));
  for(int i=0;i<40;++i)assert(!p.choose(false,false));assert(p.choose(false,false));assert(p.choose(true,false));

  puts("tap/200ms-boundary/consecutive-double/long-hold/reading/settings/chord/wrap/reset/refresh: PASS");
}
