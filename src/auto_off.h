#pragma once
#include <cstdint>

// Battery-only automatic power-off: after an idle timeout, or at low battery.
// The caller shuts down through the same path as A+B. External power never
// powers off automatically; returning to battery restarts the idle timeout.
namespace auto_off {

// Timeout codes, stored in the reader settings record. Code 0 is the default
// so a default record keeps its original bytes.
constexpr uint8_t DEFAULT_CODE = 0, NEVER_CODE = 5;
inline bool validCode(uint8_t code) { return code <= NEVER_CODE; }
// Milliseconds; 0 means the idle timeout is off.
inline uint32_t timeoutMs(uint8_t code) {
  switch (code) {
    case 0: return 30u * 60000u;
    case 1: return 10u * 60000u;
    case 2: return 15u * 60000u;
    case 3: return 60u * 60000u;
    case 4: return 120u * 60000u;
    default: return 0;
  }
}

constexpr int LOW_BATTERY = 5;            // percent, inclusive
constexpr uint32_t SAMPLE_MS = 5000;      // fuel-gauge read interval on battery
constexpr unsigned LOW_SAMPLES = 3;       // consecutive low reads before acting
constexpr uint32_t RETRY_MS = 60000;      // after a shutdown that did not complete

enum class Trigger : uint8_t { None, Idle, LowBattery };

struct Inputs {
  uint32_t now = 0;
  bool battery = false;  // a checked PMIC read shows battery only, no USB host
  bool ready = false;    // no foreground work, transfer or open radio mode
  uint32_t timeoutMs = 0;
};

class Policy {
  uint32_t lastActivity = 0, lastSample = 0, lastAttempt = 0;
  unsigned lowReads = 0;
  bool onBattery = false, sampled = false, attempted = false;

public:
  // User input and foreground work only; status-area refreshes and timer
  // wakeups must not postpone the timeout.
  void noteActivity(uint32_t now) { lastActivity = now; }
  void noteFailedShutdown(uint32_t now) { lastAttempt = now; attempted = true; }

  // True when the caller should read the fuel gauge and pass it to sample().
  bool wantsSample(const Inputs& in) const {
    return in.battery && (!sampled || uint32_t(in.now - lastSample) >= SAMPLE_MS);
  }
  // A failed read (negative) never counts toward a low-battery shutdown.
  void sample(uint32_t now, int level) {
    sampled = true; lastSample = now;
    lowReads = level >= 0 && level <= LOW_BATTERY ? lowReads + 1 : 0;
  }

  Trigger plan(const Inputs& in) {
    if (!in.battery) {
      onBattery = false; sampled = false; lowReads = 0;
      return Trigger::None;
    }
    if (!onBattery) { onBattery = true; lastActivity = in.now; }
    if (attempted) {
      if (uint32_t(in.now - lastAttempt) < RETRY_MS) return Trigger::None;
      attempted = false;
    }
    if (!in.ready) return Trigger::None;
    if (lowReads >= LOW_SAMPLES) return Trigger::LowBattery;
    if (in.timeoutMs && uint32_t(in.now - lastActivity) >= in.timeoutMs) return Trigger::Idle;
    return Trigger::None;
  }
};

} // namespace auto_off
