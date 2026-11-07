#include "auto_off.h"
#include <cassert>
#include <cstdio>

using namespace auto_off;
namespace {
constexpr uint32_t MIN = 60000;
Inputs on(uint32_t now, bool ready = true, uint32_t timeout = 30 * MIN) {
  Inputs in; in.now = now; in.battery = true; in.ready = ready; in.timeoutMs = timeout; return in;
}
Inputs external(uint32_t now) { Inputs in; in.now = now; in.ready = true; in.timeoutMs = 30 * MIN; return in; }
}

int main() {
  // Codes: 0 is the 30 minute default; 5 turns only the idle timeout off.
  assert(timeoutMs(DEFAULT_CODE) == 30 * MIN && timeoutMs(1) == 10 * MIN && timeoutMs(2) == 15 * MIN);
  assert(timeoutMs(3) == 60 * MIN && timeoutMs(4) == 120 * MIN && timeoutMs(NEVER_CODE) == 0);
  assert(validCode(0) && validCode(NEVER_CODE) && !validCode(6) && !validCode(255));

  // The timeout counts from the last activity, on battery only.
  {
    Policy p; p.noteActivity(1000);
    assert(p.plan(on(1000)) == Trigger::None);
    assert(p.plan(on(1000 + 30 * MIN - 1)) == Trigger::None);
    assert(p.plan(on(1000 + 30 * MIN)) == Trigger::Idle);
    p.noteActivity(1000 + 30 * MIN);
    assert(p.plan(on(1000 + 59 * MIN)) == Trigger::None);
    assert(p.plan(on(1000 + 60 * MIN)) == Trigger::Idle);
    // Busy work, an open N/A radio or a transfer only postpones the shutdown.
    assert(p.plan(on(1000 + 90 * MIN, false)) == Trigger::None);
    assert(p.plan(on(1000 + 90 * MIN)) == Trigger::Idle);
    // Never: no idle shutdown.
    assert(p.plan(on(1000 + 900 * MIN, true, 0)) == Trigger::None);
  }

  // External power never powers off; unplugging restarts the timeout, so a
  // long idle period on USB does not shut down the moment it is removed.
  {
    Policy p; p.noteActivity(0);
    assert(p.plan(external(0)) == Trigger::None);
    assert(p.plan(external(500 * MIN)) == Trigger::None);
    assert(p.plan(on(500 * MIN)) == Trigger::None);
    assert(p.plan(on(529 * MIN)) == Trigger::None);
    assert(p.plan(on(530 * MIN)) == Trigger::Idle);
  }

  // Low battery: three consecutive low reads, each SAMPLE_MS apart, then a
  // shutdown regardless of the idle timeout. Failed or higher reads reset it.
  {
    Policy p; p.noteActivity(0);
    uint32_t now = 0;
    auto read = [&](int level) {
      Inputs in = on(now, true, 0);
      assert(p.wantsSample(in)); p.sample(now, level);
      Trigger t = p.plan(in);
      assert(!p.wantsSample(on(now + SAMPLE_MS - 1, true, 0)));
      now += SAMPLE_MS;
      return t;
    };
    assert(read(5) == Trigger::None && read(4) == Trigger::None);
    assert(read(6) == Trigger::None);                       // reset
    assert(read(5) == Trigger::None && read(-1) == Trigger::None); // failed read resets
    assert(read(3) == Trigger::None && read(3) == Trigger::None && read(2) == Trigger::LowBattery);
    // Not ready: wait, then act without another three reads.
    assert(p.plan(on(now, false, 0)) == Trigger::None);
    assert(p.plan(on(now, true, 0)) == Trigger::LowBattery);
    // External power clears the count and the sampling schedule.
    assert(p.plan(external(now)) == Trigger::None);
    assert(p.wantsSample(on(now)));
    p.sample(now, 1);
    assert(p.plan(on(now, true, 0)) == Trigger::None);
    Inputs ext = external(now);
    assert(!p.wantsSample(ext));
  }

  // A shutdown that returned (for example, progress could not be saved) is
  // retried after RETRY_MS, not on every loop.
  {
    Policy p; p.noteActivity(0);
    assert(p.plan(on(0)) == Trigger::None);
    assert(p.plan(on(30 * MIN)) == Trigger::Idle);
    p.noteFailedShutdown(30 * MIN);
    assert(p.plan(on(30 * MIN + RETRY_MS - 1)) == Trigger::None);
    assert(p.plan(on(30 * MIN + RETRY_MS)) == Trigger::Idle);
  }

  // Elapsed time uses unsigned subtraction across the millis() wrap.
  {
    Policy p; const uint32_t start = 0xffffffffu - 5 * MIN;
    p.noteActivity(start);
    assert(p.plan(on(start)) == Trigger::None);
    assert(p.plan(on(start + 29 * MIN)) == Trigger::None);
    assert(p.plan(on(start + 30 * MIN)) == Trigger::Idle);
  }
  std::puts("auto power-off idle, external power, low battery, retry and wrap: PASS");
}
