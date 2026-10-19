#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "input.h"
#include "refresh_policy.h"
using controls::Action;
int main(){
 controls::Buttons b;
 assert(b.update(1,0,true)==Action::None);assert(b.update(0,100,true)==Action::Previous);
 assert(b.update(2,200,true)==Action::None);assert(b.update(0,300,true)==Action::Next);
 b.update(1,400,true);assert(b.update(3,450,true)==Action::None);assert(b.update(2,500,true)==Action::None);assert(b.update(0,550,true)==Action::Confirm);assert(b.update(0,600,true)==Action::None);
 b.update(2,1000,true);assert(b.update(2,1599,true)==Action::None);assert(b.update(2,1600,true)==Action::Next);assert(b.update(2,1950,true)==Action::Next);assert(b.update(2,2450,true)==Action::NextScreen);assert(b.update(0,2500,true)==Action::None);
 b.update(1,3000,false);assert(b.update(1,5000,false)==Action::Previous);assert(b.update(0,5100,false)==Action::None);
 b.reset(3,6000);assert(b.update(3,6100,true)==Action::None);assert(b.update(1,6200,true)==Action::None);assert(b.update(0,6300,true)==Action::None);
 b.update(1,UINT32_MAX-300,true);assert(b.update(1,400,true)==Action::Previous);b.update(0,500,true);
 // Busy completion: held keys, chords, rapid tapping and release debounce
 // are discarded; only a fresh press after 200ms of quiet is accepted.
 b.discardUntilReleased(6000);
 for(auto mask:{3,2,0,1,0,3,0})assert(b.update(mask,6100,true)==Action::None);
 assert(b.update(0,6299,true)==Action::None);
 assert(b.update(0,6300,true)==Action::None);
 assert(b.update(2,6310,true)==Action::None);
 assert(b.update(0,6400,true)==Action::Next);
 b.discardUntilReleased(UINT32_MAX-100);
 assert(b.update(0,UINT32_MAX-100,false)==Action::None);
 assert(b.update(0,100,false)==Action::None);
 assert(b.update(3,110,false)==Action::None);
 assert(b.update(0,120,false)==Action::Confirm);
 b.reset(0,10000);assert(b.update(3,10000,false)==Action::None);
 assert(b.update(3,11999,false)==Action::None);assert(b.update(3,12000,false)==Action::Shutdown);
 assert(b.update(3,14000,false)==Action::None);assert(b.update(0,14010,false)==Action::None);
 assert(b.update(0,14210,false)==Action::None);assert(b.update(0,14220,false)==Action::None);
 // One released key interrupts the continuous shutdown hold.
 b.reset(0,15000);b.update(3,15000,true);assert(b.update(1,16999,true)==Action::None);
 assert(b.update(3,17000,true)==Action::None);assert(b.update(3,18999,true)==Action::None);
 assert(b.update(0,19000,true)==Action::Confirm);
 assert(controls::moveSelection(0,0,1,true)==0);
 assert(controls::moveSelection(0,27,-1,false)==26);assert(controls::moveSelection(26,27,1,false)==0);
 assert(controls::moveSelection(0,27,1,true)==22);assert(controls::moveSelection(22,27,1,true)==0);
 assert(controls::moveSelection(21,27,1,true)==26);assert(controls::moveSelection(26,27,-1,true)==4);
 RefreshPolicy p;assert(p.choose(true,true));for(int i=0;i<10;++i)assert(!p.choose(false,true));assert(p.choose(false,true));
 for(int i=0;i<40;++i)assert(!p.choose(false,false));assert(p.choose(false,false));assert(p.choose(true,false));
 puts("tap/chord/release/hold/acceleration/wrap/reset/navigation/refresh policy: PASS");
}
