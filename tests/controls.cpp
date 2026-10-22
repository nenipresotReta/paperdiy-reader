#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "input.h"
#include "refresh_policy.h"
using controls::Action;
int main(){
  controls::Buttons b;

  // Single click in the library is deferred to distinguish a double click.
  assert(b.update(1,0,true)==Action::None);
  assert(b.update(0,100,true)==Action::None);
  assert(b.update(0,300,true)==Action::None);
  assert(b.update(0,401,true)==Action::Previous);
  assert(b.update(2,500,true)==Action::None);
  assert(b.update(0,600,true)==Action::None);
  assert(b.update(0,901,true)==Action::Next);

  // Double click on the same key: one page, no single selection.
  b.reset(0,1000);
  assert(b.update(1,1000,true)==Action::None);
  assert(b.update(0,1100,true)==Action::None);
  assert(b.update(1,1200,true)==Action::None);
  assert(b.update(0,1300,true)==Action::PreviousScreen);
  b.reset(0,1400);
  assert(b.update(2,1400,true)==Action::None);
  assert(b.update(0,1500,true)==Action::None);
  assert(b.update(2,1600,true)==Action::None);
  assert(b.update(0,1700,true)==Action::NextScreen);

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

  // Settings: allowRepeat=false means one toggle on release, no hold repeat.
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

  puts("tap/double-click/long-hold/reading/settings/chord/wrap/reset/refresh: PASS");
}
