#include "usb_transport.h"
#include <esp32-hal-tinyusb.h>
#include <cassert>
#include <cstdio>
#include <Arduino.h>

namespace {
bool mounted=false,suspended=false;
}
bool tud_mounted(){return mounted;}
bool tud_suspended(){return suspended;}

int main(){
  // No host: the reader may sleep.
  assert(!usb_transport::hostConnected()&&!usb_transport::blocksSleep());
  // A configured host keeps the CPU awake for the console and flashing.
  mounted=true;
  assert(usb_transport::hostConnected()&&usb_transport::blocksSleep());
  assert(usb_transport::connection().configured&&!usb_transport::connection().suspended);
  // A suspended host may resume at any moment and still blocks sleep, but it
  // is not reported as a connected console.
  mounted=false;suspended=true;
  assert(!usb_transport::hostConnected()&&usb_transport::blocksSleep());
  suspended=false;
  assert(!usb_transport::blocksSleep());
  std::puts("usb_transport connection tests passed");
}
