#pragma once
#include "delivery_store.h"
namespace delivery_store {
struct SDHooks {
  bool (*admit)(const Metadata&)=nullptr;
  void (*published)(const Receipt&,const char*)=nullptr;
};
Store& sdStore();
// App task, after local FAT mount and before exposing the N receiver. Captures
// its storage lease, creates only fixed owned directories and starts recovery.
bool beginSDStore(SDHooks hooks=SDHooks());
void pollSDStore();
// Stop before network/USB handoff. Uncommitted stage only may be removed;
// published files and uncertain recovery records remain untouched.
void stopSDStore();
// Card loss: no filesystem calls, permanently revokes this session's access.
void invalidateSDStore();
}

namespace delivery_store_sd {
delivery_store::Store& store();
void setHooks(delivery_store::SDHooks hooks);
bool mount();
void unmount();
bool mounted();
void poll();
void invalidate();
}
