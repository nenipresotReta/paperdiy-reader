#include "console.h"
#ifdef ARDUINO
#if !ARDUINO_USB_MODE && CONFIG_TINYUSB_CDC_ENABLED
#include <USB.h>
#include <esp32-hal-tinyusb.h>
#endif
namespace {
bool connected(void*){
#if !ARDUINO_USB_MODE && CONFIG_TINYUSB_CDC_ENABLED
  return tud_cdc_n_connected(0);
#else
  return bool(Serial);
#endif
}
size_t available(void*){
#if !ARDUINO_USB_MODE && CONFIG_TINYUSB_CDC_ENABLED
  return tud_cdc_n_write_available(0);
#else
  const int n=Serial.availableForWrite();return n>0?size_t(n):0;
#endif
}
size_t write(void*,const uint8_t* bytes,size_t count){
#if !ARDUINO_USB_MODE && CONFIG_TINYUSB_CDC_ENABLED
  // One FIFO attempt only: unlike USBCDC::write, no wait loop is hidden here.
  return tud_cdc_n_write(0,bytes,uint32_t(count));
#else
  // HWCDC uses its interrupt-driven ring. Call with no more than the free
  // snapshot and no competing application TX writer, so no remainder waits.
  return Serial.write(bytes,count);
#endif
}
void flush(void*){
#if !ARDUINO_USB_MODE && CONFIG_TINYUSB_CDC_ENABLED
  if(tud_cdc_n_connected(0))tud_cdc_n_write_flush(0);
#endif
  // HWCDC::write already kicks the hardware ISR. Do not wait for a host ACK.
}
uint32_t now(void*){return millis();}
void idle(void*){delay(1);}
paperdiy_console::IO makeIO(){
  paperdiy_console::IO io;io.connected=connected;io.available=available;io.write=write;
  io.flush=flush;io.now=now;io.idle=idle;return io;
}
}
ConsoleStream::ConsoleStream():writer_(makeIO()){}
size_t ConsoleStream::write(const uint8_t* bytes,size_t count){return writer_.write(bytes,count);}
void ConsoleStream::flush(){writer_.flush();}
ConsoleStream Console;
#endif
