#pragma once
#include "usb_features.h"

namespace usb_transport {
struct Connection {
  bool configured=false,suspended=false;
  Connection(bool configured_=false,bool suspended_=false):configured(configured_),suspended(suspended_){}
};
// MODE=0: USB configured state, independent of serial terminal/DTR. A USB
// reset/deconfiguration cannot establish that a cable was physically removed.
Connection connection();
bool hostConnected();
// Includes retained MSC ownership, even after host deconfiguration/suspend.
bool blocksSleep();
bool storageSupported();
// Disable SDK 1200-baud/DTR reset shortcuts before handing SD to the host.
// Re-enable only after safe eject has released raw media ownership.
void setStorageOwned(bool owned);
}
