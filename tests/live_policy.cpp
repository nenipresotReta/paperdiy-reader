#include <cassert>
#include <cstdint>
#include <cstdio>
#include "live_policy.h"

using live_policy::Inputs;
using live_policy::Machine;
using live_policy::Reason;
using live_policy::State;
using live_policy::Supply;

namespace {
Inputs at(uint32_t now, Supply supply, int battery = 80, bool connected = false) {
  Inputs in;
  in.now = now;
  in.supply = supply;
  in.battery = battery;
  in.connected = connected;
  return in;
}

void batteryDeadlines() {
  Machine m;
  m.start(at(1000, Supply::Battery));
  assert(m.state() == State::Connecting && m.radioWanted() && m.selected());
  assert(!m.step(at(1000 + live_policy::CONNECT_MS - 1, Supply::Battery)));
  assert(m.step(at(1000 + live_policy::CONNECT_MS, Supply::Battery)));
  assert(m.state() == State::Failed && m.reason() == Reason::ConnectFailed);
  assert(!m.radioWanted() && !m.selected());
  // Failed is terminal until the user selects P again.
  assert(!m.step(at(900000, Supply::External, 80, true)));

  m.start(at(0, Supply::Battery));
  assert(m.step(at(10, Supply::Battery, 80, true)) && m.state() == State::Online);
  assert(m.step(at(5000, Supply::Battery)) && m.state() == State::Reconnecting);
  assert(!m.step(at(5000 + live_policy::RECONNECT_MS - 1, Supply::Battery)));
  assert(m.step(at(5000 + live_policy::RECONNECT_MS - 1, Supply::Battery, 80, true)));
  assert(m.state() == State::Online);
  assert(m.step(at(100000, Supply::Battery)) && m.state() == State::Reconnecting);
  assert(m.step(at(100000 + live_policy::RECONNECT_MS, Supply::Battery)));
  assert(m.state() == State::Failed && m.reason() == Reason::ConnectionLost);
}

void externalBackoff() {
  Machine m;
  m.start(at(0, Supply::External));
  assert(m.step(at(live_policy::CONNECT_MS, Supply::External)));
  assert(m.state() == State::Waiting && !m.radioWanted() && m.selected());
  assert(m.retryIn(live_policy::CONNECT_MS) == live_policy::BACKOFF_MS[0]);
  uint32_t now = live_policy::CONNECT_MS;
  // 1, 2, then 5 minutes; the last step repeats.
  const uint32_t expected[] = {60000, 120000, 300000, 300000};
  for (uint32_t wait : expected) {
    assert(m.state() == State::Waiting && m.retryIn(now) == wait);
    assert(!m.step(at(now + wait - 1, Supply::External)));
    now += wait;
    assert(m.step(at(now, Supply::External)) && m.state() == State::Connecting);
    now += live_policy::CONNECT_MS;
    assert(m.step(at(now, Supply::External)) && m.state() == State::Waiting);
  }
  // A success resets the schedule; a later loss retries for a full minute.
  now += m.retryIn(now);
  assert(m.step(at(now, Supply::External)) && m.state() == State::Connecting);
  assert(m.step(at(now + 1, Supply::External, 80, true)) && m.state() == State::Online);
  now += 10;
  assert(m.step(at(now, Supply::External)) && m.state() == State::Reconnecting);
  assert(!m.step(at(now + live_policy::RECONNECT_MS - 1, Supply::External)));
  assert(m.step(at(now + live_policy::RECONNECT_MS, Supply::External)));
  assert(m.state() == State::Waiting && m.retryIn(now + live_policy::RECONNECT_MS) == live_policy::BACKOFF_MS[0]);
}

void unplugRestartsDeadline() {
  Machine m;
  m.start(at(0, Supply::External));
  assert(m.step(at(1, Supply::External, 80, true)) && m.state() == State::Online);
  assert(m.step(at(2, Supply::External)) && m.state() == State::Reconnecting);
  // A loss tolerated on external power does not count once unplugged.
  assert(!m.step(at(1 + live_policy::RECONNECT_MS, Supply::External)));
  assert(m.step(at(2 + live_policy::RECONNECT_MS, Supply::External)) && m.state() == State::Waiting);
  assert(m.step(at(70000, Supply::Battery)) && m.state() == State::Reconnecting);
  assert(!m.step(at(70000 + live_policy::RECONNECT_MS - 1, Supply::Battery)));
  assert(m.step(at(70000 + live_policy::RECONNECT_MS, Supply::Battery)));
  assert(m.state() == State::Failed && m.reason() == Reason::ConnectionLost);

  // Unplugged while still connecting: 30 s from the unplug, not from start.
  m.start(at(0, Supply::External));
  assert(!m.step(at(25000, Supply::External)));
  assert(!m.step(at(25000, Supply::Battery)));
  assert(m.state() == State::Connecting);
  assert(!m.step(at(25000 + live_policy::CONNECT_MS - 1, Supply::Battery)));
  assert(m.step(at(25000 + live_policy::CONNECT_MS, Supply::Battery)));
  assert(m.reason() == Reason::ConnectFailed);
}

void lowBatteryPause() {
  Machine m;
  m.start(at(0, Supply::Battery, 49));
  assert(m.state() == State::Paused && m.reason() == Reason::LowBattery);
  assert(m.selected() && !m.radioWanted());
  // Only external power resumes; a battery reading above 50 % does not.
  assert(!m.step(at(10, Supply::Battery, 80)));
  assert(!m.step(at(10, Supply::Unknown, 80)));
  assert(m.step(at(20, Supply::External, 49)) && m.state() == State::Connecting);
  assert(m.reason() == Reason::None);
  assert(m.step(at(30, Supply::External, 49, true)) && m.state() == State::Online);
  // Unplugged below the threshold pauses again, even while connected.
  assert(m.step(at(40, Supply::Battery, 49, true)) && m.state() == State::Paused);
  // Exactly 50 % stays online; unknown readings never pause.
  m.start(at(0, Supply::Battery, 50));
  assert(m.state() == State::Connecting);
  assert(!m.step(at(1, Supply::Battery, -1)));
  assert(!m.step(at(2, Supply::Battery, 101)));
  assert(m.state() == State::Connecting);
  // After a resume the previous connection makes the next attempt a reconnect.
  m.start(at(0, Supply::Battery, 80));
  assert(m.step(at(1, Supply::Battery, 80, true)));
  assert(m.step(at(2, Supply::Battery, 40)) && m.state() == State::Paused);
  assert(m.step(at(3, Supply::External, 40)) && m.state() == State::Reconnecting);
}

void unknownSupply() {
  // A failed PMIC read neither applies battery deadlines nor pauses P.
  Machine m;
  m.start(at(0, Supply::Unknown, 10));
  assert(m.state() == State::Connecting);
  assert(m.step(at(live_policy::CONNECT_MS, Supply::Unknown, 10)));
  assert(m.state() == State::Waiting);
}

void stopAndFail() {
  Machine m;
  m.start(at(0, Supply::External));
  m.fail(Reason::NoSender, 5);
  assert(m.state() == State::Failed && m.reason() == Reason::NoSender && !m.selected());
  m.stop();
  assert(m.state() == State::Inactive && m.reason() == Reason::None && !m.selected());
  assert(!m.step(at(1000000, Supply::Battery)));
}

void clockWrap() {
  Machine m;
  const uint32_t start = UINT32_MAX - 1000;
  m.start(at(start, Supply::Battery));
  assert(!m.step(at(start + live_policy::CONNECT_MS - 1, Supply::Battery)));
  assert(m.step(at(start + live_policy::CONNECT_MS, Supply::Battery)));
  assert(m.reason() == Reason::ConnectFailed);
  m.start(at(start, Supply::External));
  assert(m.step(at(start + live_policy::CONNECT_MS, Supply::External)) && m.state() == State::Waiting);
  const uint32_t retry = start + live_policy::CONNECT_MS + live_policy::BACKOFF_MS[0];
  assert(!m.step(at(retry - 1, Supply::External)));
  assert(m.step(at(retry, Supply::External)) && m.state() == State::Connecting);
}
}  // namespace

int main() {
  batteryDeadlines();
  externalBackoff();
  unplugRestartsDeadline();
  lowBatteryPause();
  unknownSupply();
  stopAndFail();
  clockWrap();
  std::puts("live_policy tests passed");
}
