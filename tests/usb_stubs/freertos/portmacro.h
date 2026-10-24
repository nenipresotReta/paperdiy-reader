#pragma once
#include <cassert>
struct portMUX_TYPE {bool locked;};
#define portMUX_INITIALIZER_UNLOCKED {false}
inline void portENTER_CRITICAL(portMUX_TYPE* mux){assert(!mux->locked);mux->locked=true;}
inline void portEXIT_CRITICAL(portMUX_TYPE* mux){assert(mux->locked);mux->locked=false;}
