#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "idle_policy.h"
#include "input.h"

using controls::Action;
using idle_power::Inputs;
using idle_power::Plan;
using idle_power::Policy;

namespace {
constexpr Inputs ready{true, true, false, false};

void expect(Plan plan, bool idle, bool sleep) {
  assert(plan.idle == idle);
  assert(plan.sleep == sleep);
  assert(plan.sleepMs == (sleep ? idle_power::MAX_SLEEP_MS : 0));
}

Inputs withButtons(const controls::Buttons& buttons, uint8_t rawMask = 0) {
  return {true, buttons.idle(), rawMask != 0, false};
}

void policyBoundaries() {
  Policy policy;
  policy.noteActivity(100);
  expect(policy.plan(1099, ready), false, false);
  expect(policy.plan(1100, ready), true, true);
  // A timer wake or maintenance poll must not restart the idle grace period.
  expect(policy.plan(2100, ready), true, true);
  expect(policy.plan(3100, ready), true, true);

  expect(policy.plan(3100, {false, true, false, false}), false, false);
  expect(policy.plan(3100, {true, false, false, false}), false, false);
  // A raw press must block sleep even before hardware debounce accepts it.
  expect(policy.plan(3100, {true, true, true, false}), false, false);
  expect(policy.plan(3100, {true, true, false, true}), true, false);
  // A battery-powered AP/STA stays connected without giving up CPU throttling.
  expect(policy.plan(3100, {true, true, false, false, true}), true, false);
  expect(policy.plan(3100, {true, true, true, false, true}), false, false);
  // Turning the network off restores normal battery idle sleep immediately.
  expect(policy.plan(3100, ready), true, true);

  policy.noteActivity(3100);
  expect(policy.plan(4099, ready), false, false);
  expect(policy.plan(4100, ready), true, true);

  policy.noteSleepFailure(4100);
  expect(policy.plan(4100, ready), true, false);
  expect(policy.plan(9099, ready), true, false);
  expect(policy.plan(9100, ready), true, true);
  // Repeated rejection extends the backoff; real activity does not cancel it.
  policy.noteSleepFailure(9200);
  policy.noteSleepFailure(10200);
  policy.noteActivity(10300);
  expect(policy.plan(11299, ready), false, false);
  expect(policy.plan(11300, ready), true, false);
  expect(policy.plan(15199, ready), true, false);
  expect(policy.plan(15200, ready), true, true);
}

void counterRollover() {
  Policy policy;
  constexpr uint32_t base = UINT32_MAX - 500;
  policy.noteActivity(base);
  expect(policy.plan(uint32_t(base + 999), ready), false, false);
  expect(policy.plan(uint32_t(base + 1000), ready), true, true);

  Policy failing;
  failing.noteActivity(base - 1000);
  failing.noteSleepFailure(base);
  expect(failing.plan(uint32_t(base + 4999), ready), true, false);
  expect(failing.plan(uint32_t(base + 5000), ready), true, true);
  // A completed backoff must not reactivate on a later millis cycle.
  failing.noteActivity(base - 1000);
  expect(failing.plan(base, ready), true, true);
}
void storageOwnershipPinsActiveClock(){
  Policy policy;policy.noteActivity(0);
  // No byte-delta activity arrives while a host is idle or while its first
  // callback runs in another task. Ownership alone must prevent throttling.
  for(uint32_t time:{1000u,60000u,600000u,UINT32_MAX}){
    expect(policy.plan(time,{true,true,false,true,false,true}),false,false);
    // A suspended/reset host or a Fault still retains the same ownership.
    expect(policy.plan(time,{true,true,false,false,false,true}),false,false);
  }
  // Only MSC ownership changes the clock rule: ordinary CDC/AP/STA retain
  // CPU throttling, and successful release restores the original policy.
  expect(policy.plan(600000,{true,true,false,true,false,false}),true,false);
  expect(policy.plan(600000,{true,true,false,false,true,false}),true,false);
  expect(policy.plan(600000,ready),true,true);
}

void firstPressAfterSleep() {
  for (uint8_t key : {uint8_t(1), uint8_t(2)}) {
    Policy policy;
    controls::Buttons buttons;
    policy.noteActivity(0);
    expect(policy.plan(1000, withButtons(buttons)), true, true);

    // Runtime wakes and feeds the first sample to the existing state machine.
    // Resetting/discarding the state here would swallow this first action.
    expect(policy.plan(1500, withButtons(buttons, key)), false, false);
    policy.noteActivity(1500);
    assert(buttons.update(key, 1500, false) == Action::None);
    assert(!buttons.idle());
    expect(policy.plan(1600, withButtons(buttons, key)), false, false);
    assert(buttons.update(0, 1600, false) ==
           (key == 1 ? Action::Previous : Action::Next));
    assert(buttons.idle());
    policy.noteActivity(1600);
    assert(buttons.update(0, 1610, false) == Action::None);
    expect(policy.plan(2599, withButtons(buttons)), false, false);
    expect(policy.plan(2600, withButtons(buttons)), true, true);
  }
}

void holdAndChordAfterSleep() {
  Policy policy;
  controls::Buttons buttons;
  expect(policy.plan(1000, withButtons(buttons)), true, true);
  policy.noteActivity(2000);
  assert(buttons.update(2, 2000, true) == Action::None);
  assert(buttons.update(2, 2599, true) == Action::None);
  assert(buttons.update(2, 2600, true) == Action::Next);
  assert(!buttons.idle());
  assert(buttons.update(2, 3400, true) == Action::Next);
  expect(policy.plan(4000, withButtons(buttons)), false, false);
  assert(buttons.update(0, 4010, true) == Action::None);
  assert(buttons.idle());

  // A staggered chord release confirms exactly once and cannot sleep midway.
  buttons.reset(0, 5000);
  assert(buttons.update(1, 5000, false) == Action::None);
  assert(buttons.update(3, 5010, false) == Action::None);
  assert(buttons.update(2, 5100, false) == Action::None);
  assert(!buttons.idle());
  expect(policy.plan(5100, withButtons(buttons)), false, false);
  assert(buttons.update(0, 5200, false) == Action::Confirm);
  assert(buttons.idle());
  assert(buttons.update(0, 5210, false) == Action::None);

  // Continuous A+B still requests shutdown at two seconds, then guards release.
  assert(buttons.update(3, 6000, false) == Action::None);
  assert(buttons.update(3, 7999, false) == Action::None);
  expect(policy.plan(7999, withButtons(buttons)), false, false);
  assert(buttons.update(3, 8000, false) == Action::Shutdown);
  assert(!buttons.idle());
  assert(buttons.update(0, 8010, false) == Action::None);
  assert(buttons.update(0, 8209, false) == Action::None);
  assert(!buttons.idle());
  assert(buttons.update(0, 8210, false) == Action::None);
  assert(buttons.idle());
  assert(buttons.update(0, 8220, false) == Action::None);
}

void loadingReleaseGuard() {
  Policy policy;
  controls::Buttons buttons;
  buttons.reset(3, 1000);
  expect(policy.plan(2000, withButtons(buttons)), false, false);
  assert(buttons.update(3, 2000, true) == Action::None);
  assert(buttons.update(1, 2010, true) == Action::None);
  assert(!buttons.idle());
  assert(buttons.update(0, 2020, true) == Action::None);
  assert(buttons.idle());

  buttons.discardUntilReleased(3000);
  assert(buttons.update(0, 3010, true) == Action::None);
  assert(buttons.update(0, 3209, true) == Action::None);
  expect(policy.plan(3209, withButtons(buttons)), false, false);
  // A new press restarts the required continuously released interval.
  assert(buttons.update(2, 3210, true) == Action::None);
  assert(buttons.update(0, 3220, true) == Action::None);
  assert(buttons.update(0, 3419, true) == Action::None);
  assert(!buttons.idle());
  assert(buttons.update(0, 3420, true) == Action::None);
  assert(buttons.idle());
  expect(policy.plan(3420, withButtons(buttons)), true, true);
  assert(buttons.update(2, 3500, true) == Action::None);
  assert(buttons.update(0, 3600, true) == Action::None);
  assert(!buttons.idle());
  assert(buttons.update(0, 3901, true) == Action::Next);
  assert(buttons.idle());

  buttons.discardUntilReleased(UINT32_MAX - 100);
  assert(buttons.update(0, UINT32_MAX - 100, true) == Action::None);
  assert(buttons.update(0, 98, true) == Action::None);
  assert(!buttons.idle());
  assert(buttons.update(0, 99, true) == Action::None);
  assert(buttons.idle());
}
} // namespace

int main() {
  policyBoundaries();
  counterRollover();
  storageOwnershipPinsActiveClock();
  firstPressAfterSleep();
  holdAndChordAfterSleep();
  loadingReleaseGuard();
  puts("idle/USB/backoff/rollover/wake input/hold/chord/release guard: PASS");
}
