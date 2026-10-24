#pragma once
#include "console_policy.h"
#ifdef ARDUINO
#include <Arduino.h>

// Native TX bypasses USBCDC::write's unbounded full-FIFO loop. RX and hardware
// setup continue to use Serial; logging is best effort, never a storage proof.
class ConsoleStream:public Stream {
 public:
  ConsoleStream();
  using Print::write;
  size_t write(uint8_t byte)override{return write(&byte,1);}
  size_t write(const uint8_t* bytes,size_t count)override;
  int available()override{return Serial.available();}
  int read()override{return Serial.read();}
  int peek()override{return Serial.peek();}
  void flush()override;
  paperdiy_console::Writer& writer(){return writer_;}
 private:
  paperdiy_console::Writer writer_;
};
extern ConsoleStream Console;
namespace paperdiy_console {
class Frame {
 public:
  explicit Frame(uint32_t limit=FRAME_MS):owned_(Console.writer().beginFrame(limit)){}
  ~Frame(){finish();}
  bool good()const{return Console.writer().good();}
  void finish(){if(owned_){Console.writer().endFrame();owned_=false;}}
  Frame(const Frame&)=delete;Frame& operator=(const Frame&)=delete;
 private:bool owned_;
};
}
#else
// Existing native integration tests supply their own Serial recorder. The
// production deadline/FIFO policy is tested independently with a fake clock.
#define Console Serial
#endif
