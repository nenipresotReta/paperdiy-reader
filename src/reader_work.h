#pragma once
#include <cstdint>

namespace reader_work {
constexpr uint32_t INDEX_INTERVAL_MS=50, DRAW_INTERVAL_MS=8;

// Keep this object across the entire indexing pass, including short pages.
// Scheduling cannot affect layout decisions; sleeping time is not work time.
class Budget {
  uint32_t resumedAt, interval;
public:
  Budget(uint32_t now,uint32_t intervalMs):resumedAt(now),interval(intervalMs){}
  template<class Clock,class Yield>
  bool cooperate(Clock clock,Yield yield){
    if(uint32_t(clock()-resumedAt)<interval)return false;
    yield();resumedAt=clock();return true;
  }
};
}
