#include "usb_transport.h"
#ifdef ARDUINO
#include <Arduino.h>
#endif

#if defined(ARDUINO) && !ARDUINO_USB_MODE
#include <USB.h>
#include <esp32-hal-tinyusb.h>
namespace usb_transport {
Connection connection(){return {tud_mounted(),tud_suspended()};}
bool hostConnected(){return connection().configured;}
bool blocksSleep(){const auto c=connection();return c.configured||c.suspended;}
}
#elif defined(ARDUINO) && ARDUINO_USB_MODE
#include <HWCDC.h>
namespace usb_transport {
Connection connection(){return {HWCDC::isPlugged(),false};}
bool hostConnected(){return HWCDC::isPlugged();}
bool blocksSleep(){return hostConnected();}
}
#else
namespace usb_transport {
Connection connection(){return {};}
bool hostConnected(){return false;}
bool blocksSleep(){return false;}
}
#endif
