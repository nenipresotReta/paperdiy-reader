#pragma once
#include <cstdint>
#include "reader_settings.h"
struct RefreshPolicy {
  unsigned partials=0,fulls=0;
  bool choose(bool sceneChange,bool reading){
    const unsigned mode=reader_settings::current().refresh;
    const unsigned limit=mode==0?(reading?4u:16u):mode==2?(reading?24u:80u):(reading?10u:40u);
    bool full=sceneChange||partials>=limit;
    if(full){partials=0;++fulls;}else ++partials;
    return full;
  }
};
