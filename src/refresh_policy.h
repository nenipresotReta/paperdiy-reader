#pragma once
#include <cstdint>
struct RefreshPolicy {
  unsigned partials=0,fulls=0;
  bool choose(bool sceneChange,bool reading){
    bool full=sceneChange||partials>=(reading?10u:40u);
    if(full){partials=0;++fulls;}else ++partials;
    return full;
  }
};
