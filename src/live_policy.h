#pragma once
#include <cstdint>

// P keeps one STA connection and the LAN receiver up outside Settings so a
// paired sender can replace Push at any time. This policy only decides when
// the radio should run; the caller owns Wi-Fi, NVS and the receiver.
namespace live_policy {

constexpr uint32_t CONNECT_MS = 30000;    // first association, on battery
constexpr uint32_t RECONNECT_MS = 60000;  // after a connection was lost, on battery
constexpr uint32_t BACKOFF_MS[] = {60000, 120000, 300000};
constexpr unsigned BACKOFF_STEPS = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]);
constexpr int LOW_BATTERY_PERCENT = 50;

// Unknown is a failed PMIC read. It neither pauses nor resumes P and never
// applies the battery-only connection deadlines.
enum class Supply : uint8_t { Unknown, Battery, External };

enum class State : uint8_t {
  Inactive,      // P not selected
  Connecting,    // radio on, never associated in this attempt
  Online,        // associated
  Reconnecting,  // radio on, association lost
  Waiting,       // external power: radio off until the next retry
  Paused,        // battery below the threshold: radio off, P stays selected
  Failed         // radio off, P no longer selected; reason() says why
};

enum class Reason : uint8_t { None, NoWifi, NoSender, NoCard, ConnectFailed, ConnectionLost, LowBattery };

struct Inputs {
  uint32_t now = 0;
  Supply supply = Supply::Unknown;
  int battery = -1;  // percent; outside 0..100 means unknown
  bool connected = false;
};

class Machine {
  State state_ = State::Inactive;
  Reason reason_ = Reason::None;
  Supply supply_ = Supply::Unknown;
  uint32_t since_ = 0, retryAt_ = 0;
  unsigned backoff_ = 0;
  bool everConnected_ = false;

  static bool lowBattery(const Inputs& in) {
    return in.supply == Supply::Battery && in.battery >= 0 && in.battery <= 100 &&
           in.battery < LOW_BATTERY_PERCENT;
  }
  void enter(State next, uint32_t now) { state_ = next; since_ = now; }
  void attempt(uint32_t now) { enter(everConnected_ ? State::Reconnecting : State::Connecting, now); }
  void wait(uint32_t now) {
    enter(State::Waiting, now);
    retryAt_ = now + BACKOFF_MS[backoff_];
    if (backoff_ + 1 < BACKOFF_STEPS) ++backoff_;
  }

 public:
  State state() const { return state_; }
  Reason reason() const { return reason_; }
  // The persisted intent: P stays selected across shutdown and while paused.
  bool selected() const { return state_ != State::Inactive && state_ != State::Failed; }
  bool radioWanted() const {
    return state_ == State::Connecting || state_ == State::Online || state_ == State::Reconnecting;
  }
  // Remaining backoff while Waiting, for display only.
  uint32_t retryIn(uint32_t now) const {
    return state_ == State::Waiting && int32_t(retryAt_ - now) > 0 ? retryAt_ - now : 0;
  }

  // User selection or boot restore, after the caller checked Wi-Fi config,
  // a paired sender and the card. A low battery starts paused.
  void start(const Inputs& in) {
    reason_ = Reason::None;
    backoff_ = 0;
    everConnected_ = false;
    supply_ = in.supply;
    if (lowBattery(in)) {
      enter(State::Paused, in.now);
      reason_ = Reason::LowBattery;
    } else {
      enter(State::Connecting, in.now);
    }
  }
  // Ends P and clears its intent; the reason stays visible until stop/start.
  void fail(Reason why, uint32_t now) {
    enter(State::Failed, now);
    reason_ = why;
  }
  void stop() {
    state_ = State::Inactive;
    reason_ = Reason::None;
  }

  // Returns true when state() or reason() changed.
  bool step(const Inputs& in) {
    const State oldState = state_;
    const Reason oldReason = reason_;
    const bool unplugged = supply_ != Supply::Battery && in.supply == Supply::Battery;
    if (in.supply != Supply::Unknown) supply_ = in.supply;
    const bool battery = in.supply == Supply::Battery;
    const uint32_t elapsed = in.now - since_;

    switch (state_) {
      case State::Inactive:
      case State::Failed:
        break;
      case State::Paused:
        if (in.supply == Supply::External) {
          reason_ = Reason::None;
          backoff_ = 0;
          attempt(in.now);
        }
        break;
      default:
        if (lowBattery(in)) {
          enter(State::Paused, in.now);
          reason_ = Reason::LowBattery;
          break;
        }
        // Battery deadlines count from the moment external power went away,
        // never from an older disconnect that was tolerated while plugged in.
        if (unplugged && state_ != State::Online) {
          if (state_ == State::Waiting) attempt(in.now);
          else since_ = in.now;
          break;
        }
        if (state_ == State::Waiting) {
          if (int32_t(in.now - retryAt_) >= 0) attempt(in.now);
          break;
        }
        if (in.connected) {
          if (state_ != State::Online) {
            enter(State::Online, in.now);
            everConnected_ = true;
            backoff_ = 0;
          }
          break;
        }
        if (state_ == State::Online) {
          enter(State::Reconnecting, in.now);
          break;
        }
        {
          const bool reconnecting = state_ == State::Reconnecting;
          const uint32_t limit = reconnecting ? RECONNECT_MS : CONNECT_MS;
          if (elapsed < limit) break;
          if (battery) fail(reconnecting ? Reason::ConnectionLost : Reason::ConnectFailed, in.now);
          else wait(in.now);
        }
        break;
    }
    return state_ != oldState || reason_ != oldReason;
  }
};

}  // namespace live_policy
