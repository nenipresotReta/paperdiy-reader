#pragma once

namespace card_sleep {
// GPIO wake configuration shares the interrupt trigger register on ESP32-S3.
// Disable the card ISR before switching to level wake, and restore edge mode
// before enabling it again. The owner remains responsible for disabling the
// global sleep wake sources before finish(). All methods run on the app task.
template<class Backend> class Window {
 public:
  Window(Backend& backend,int initialLevel):backend_(backend),initialLevel_(initialLevel){}
  Window(const Window&)=delete;
  Window& operator=(const Window&)=delete;
  ~Window(){finish();}
  bool begin(){
    if(active_)return false;
    active_=true;
    // Interrupts cannot prove that the same card survived this interval. Drop
    // fingerprints without generating a fake insertion or remount event.
    backend_.revokeProofs();
    return backend_.disableInterrupt()&&backend_.armWake(initialLevel_);
  }
  bool finish(){
    if(!active_)return restored_;
    const bool disarmed=backend_.disarmWake();
    const bool edge=backend_.restoreEdge();
    // Never re-enable an ISR while the pin could still be level-triggered.
    const bool enabled=edge&&backend_.enableInterrupt();
    restored_=disarmed&&edge&&enabled;
    if(!restored_)backend_.revokeTrust();
    if(backend_.level()!=initialLevel_)backend_.signalChange();
    active_=false;return restored_;
  }
 private:
  Backend& backend_;
  int initialLevel_;
  bool active_=false,restored_=true;
};
}
