#pragma once
#include <cstdint>

// An accepted N batch keeps the receiver up after the reader leaves Settings,
// until the batch ends. This policy only decides when that time is over; the
// caller owns Wi-Fi, the receiver and the status icon.
namespace transfer_policy {

constexpr uint32_t IDLE_MS = 60000;     // no authenticated sender request
constexpr uint32_t RECEIPT_MS = 10000;  // lets the sender read the final result

enum class Batch : uint8_t { Pending, Finished, Failed };
enum class Outcome : uint8_t { None, Finished, Failed };

class Machine {
  enum class State : uint8_t { Inactive, Running, Holding };
  State state_ = State::Inactive;
  Outcome outcome_ = Outcome::None;
  uint32_t ended_ = 0;

public:
  void start() { state_ = State::Running; outcome_ = Outcome::None; }
  // The reader returned to N in Settings, or the user stopped N.
  void stop() { state_ = State::Inactive; }
  bool running() const { return state_ != State::Inactive; }
  Outcome outcome() const { return outcome_; }
  // The reader was busy opening a book, so the sender could not read its result.
  void hold(uint32_t now) { if (state_ == State::Holding) ended_ = now; }

  // Returns true once, when the receiver should stop.
  bool step(Batch batch, uint32_t now, uint32_t lastRequest) {
    if (state_ == State::Running) {
      if (batch == Batch::Pending && uint32_t(now - lastRequest) < IDLE_MS) return false;
      outcome_ = batch == Batch::Finished ? Outcome::Finished : Outcome::Failed;
      state_ = State::Holding;
      ended_ = now;
      return false;
    }
    if (state_ != State::Holding || uint32_t(now - ended_) < RECEIPT_MS) return false;
    state_ = State::Inactive;
    return true;
  }
};

}  // namespace transfer_policy
