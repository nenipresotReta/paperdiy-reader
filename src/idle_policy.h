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

    if (!inputs.stableScene || !inputs.inputIdle || inputs.rawPressed ||
        uint32_t(now - lastActivity) < IDLE_DELAY_MS)
      return {};

    // USB debugging and failed sleep attempts still permit idle CPU throttling.
    bool sleep = !inputs.usbConnected && !failurePending;
    return {true, sleep, sleep ? MAX_SLEEP_MS : 0};
  }
};

} // namespace idle_power
