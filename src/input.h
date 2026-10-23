#pragma once
#include <cstdint>
#include "reader_settings.h"
#include <cstddef>
namespace controls {
enum class Action {None,Previous,Next,PreviousScreen,NextScreen,Confirm,Shutdown};
inline uint8_t navigationMask(uint8_t physicalMask,bool rotated,bool settings){
  // Settings keeps its two controls beside the same physical buttons in both
  // orientations. Reading/library navigation follows the rotated page.
  return rotated&&!settings
      ? uint8_t(((physicalMask&1)?2:0)|((physicalMask&2)?1:0)) : physicalMask;
}
class Buttons {
  uint8_t held=0,pending=0;uint32_t since=0,lastRepeat=0,pendingAt=0;bool repeated=false,chord=false,suppressed=false;
  bool releaseGuard=false,releaseSeen=false;uint32_t releasedAt=0;
  uint32_t doubleMs() const {return reader_settings::current().doubleMs;}
public:
  bool idle() const {return !held&&!chord&&!suppressed&&!releaseGuard&&!pending;}
  void reset(uint8_t mask,uint32_t now){held=0;pending=0;since=lastRepeat=pendingAt=now;repeated=chord=false;suppressed=mask!=0;releaseGuard=releaseSeen=false;}
  // After blocking work, require a continuously released interval. This also
  // lets the hardware debounce state catch up before accepting a fresh press.
  void discardUntilReleased(uint32_t now){reset(0,now);releaseGuard=true;}
  Action update(uint8_t mask,uint32_t now,bool library,bool allowRepeat=true){
    if(releaseGuard){
      if(mask)releaseSeen=false;
      else if(!releaseSeen){releaseSeen=true;releasedAt=now;}
      else if(now-releasedAt>=200){releaseGuard=false;releaseSeen=false;}
      return Action::None;
    }
    if(suppressed){if(!mask)suppressed=false;return Action::None;}
    if(pending){
      if(now-pendingAt>doubleMs()||(mask&&mask!=pending)){
        uint8_t released=pending;pending=0;
        return released==1?Action::Previous:Action::Next;
      }
    }
    if(chord){
      if(!mask){chord=false;held=0;return Action::Confirm;}
      if(mask==3&&held!=3){held=3;since=now;}
      if(mask==3&&now-since>=2000){discardUntilReleased(now);return Action::Shutdown;}
      // Releasing one key cancels the hold timer; a staggered release still
      // confirms once both keys are up.
      if(mask!=3){since=now;held=mask;}
      return Action::None;
    }
    if(mask==3){chord=true;held=3;since=now;return Action::None;}
    if(!held){if(mask){held=mask;since=lastRepeat=now;repeated=false;}return Action::None;}
    if(!mask){
      uint8_t released=held;held=0;
      if(repeated)return Action::None;
      if(library){
        if(pending==released&&now-pendingAt<=doubleMs()){
          pending=0;
          return released==1?Action::PreviousScreen:Action::NextScreen;
        }
        pending=released;pendingAt=now;return Action::None;
      }
      return released==1?Action::Previous:Action::Next;
    }
    if(mask!=held){held=mask;since=lastRepeat=now;repeated=false;return Action::None;}
    uint32_t elapsed=now-since;
    if(elapsed>=600&&(!repeated||now-lastRepeat>=350)){
      if(!allowRepeat)return Action::None;
      repeated=true;lastRepeat=now;
      return held==1?Action::Previous:Action::Next;
    }
    return Action::None;
  }
};
constexpr unsigned LIBRARY_ROWS=22;
inline size_t moveSelection(size_t selected,size_t count,int direction,bool screen){
  if(!count)return 0;
  if(!screen){if(direction<0)return selected?selected-1:count-1;return selected+1<count?selected+1:0;}
  size_t pages=(count+LIBRARY_ROWS-1)/LIBRARY_ROWS,pg=selected/LIBRARY_ROWS;
  pg=direction<0?(pg?pg-1:pages-1):(pg+1)%pages;
  size_t target=pg*LIBRARY_ROWS+selected%LIBRARY_ROWS;return target<count?target:count-1;
}
}
