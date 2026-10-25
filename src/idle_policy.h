#pragma once
#include <cstdint>

namespace idle_power {

constexpr uint32_t IDLE_DELAY_MS = 1000;
constexpr uint32_t MAX_SLEEP_MS = 1000;
constexpr uint32_t FAILURE_BACKOFF_MS = 5000;

struct Inputs {
  bool stableScene;
  bool inputIdle;
  bool rawPressed;
  bool usbConnected;
  bool radioActive;
  bool storageOwned;
  constexpr Inputs(bool stable, bool idle, bool pressed, bool usb, bool radio=false,bool storage=false)
      : stableScene(stable), inputIdle(idle), rawPressed(pressed),
        usbConnected(usb), radioActive(radio),storageOwned(storage) {}
};

struct Plan {
  bool idle;
  bool sleep;
  uint32_t sleepMs;
  constexpr Plan(bool isIdle = false, bool shouldSleep = false, uint32_t duration = 0)
      : idle(isIdle), sleep(shouldSleep), sleepMs(duration) {}
};

class Policy {
  uint32_t lastActivity = 0;
  uint32_t lastSleepFailure = 0;
  bool failurePending = false;

public:
  // Only actual interaction or completed foreground work resets this clock.
  // Periodic maintenance and timer wakeups must not keep the device awake.
  void noteActivity(uint32_t now) { lastActivity = now; }

  void noteSleepFailure(uint32_t now) {
    lastSleepFailure = now;
    failurePending = true;
  }

  Plan plan(uint32_t now, const Inputs& inputs) {
    // Expire the backoff once rather than reviving an old failure when millis
    // eventually wraps. All elapsed intervals use unsigned subtraction.
    if (failurePending && uint32_t(now - lastSleepFailure) >= FAILURE_BACKOFF_MS)
      failurePending = false;

    // Raw MSC callbacks run in the USB task, outside application activity
    // accounting. Hold full speed for the complete ownership interval rather
    // than downclocking during a callback or before the first burst is noticed.
    if (inputs.storageOwned || !inputs.stableScene || !inputs.inputIdle || inputs.rawPressed ||
        uint32_t(now - lastActivity) < IDLE_DELAY_MS)
      return {};

    // Manual light sleep suspends Wi-Fi; a live AP/STA must keep servicing the
    // radio. It can still throttle the idle CPU, just like USB debugging.
    bool sleep = !inputs.usbConnected && !inputs.radioActive && !failurePending;
    return {true, sleep, sleep ? MAX_SLEEP_MS : 0};
  }
};

// The existing cache-index/write background may sleep on the settings screen;
// the content-prefetch task may not. Keeping this rule separate prevents a
// prefetch from being hidden behind the settings exception.
inline bool backgroundAllowsSleep(bool sdBackground, bool contentPrefetch, bool settings) {
  return (!sdBackground || settings) && !contentPrefetch;
}

} // namespace idle_power
