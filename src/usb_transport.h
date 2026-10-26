#pragma once

// USB is the serial console and the flashing path only. The SDK's 1200-baud
// reboot into the ROM download mode stays enabled: it is how the reader is
// flashed without the BOOT key.
namespace usb_transport {
struct Connection {
  bool configured=false,suspended=false;
  Connection(bool configured_=false,bool suspended_=false):configured(configured_),suspended(suspended_){}
};
// MODE=0: USB configured state, independent of serial terminal/DTR. A USB
// reset/deconfiguration cannot establish that a cable was physically removed.
Connection connection();
bool hostConnected();
// A configured or suspended host keeps the processor out of light sleep.
bool blocksSleep();
}
