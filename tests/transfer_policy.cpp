#include <cassert>
#include <cstdint>
#include <cstdio>
#include "transfer_policy.h"

using transfer_policy::Batch;
using transfer_policy::Machine;
using transfer_policy::Outcome;

namespace {
void finishedBatchWaitsForTheSenderThenStops() {
  Machine m;
  assert(!m.running() && !m.step(Batch::Finished, 1000, 1000));
  m.start();
  assert(m.running() && !m.step(Batch::Pending, 1000, 1000));
  assert(!m.step(Batch::Finished, 2000, 1500) && m.outcome() == Outcome::Finished);
  assert(m.running() && !m.step(Batch::Finished, 2000 + transfer_policy::RECEIPT_MS - 1, 1500));
  assert(m.step(Batch::Finished, 2000 + transfer_policy::RECEIPT_MS, 1500));
  assert(!m.running() && m.outcome() == Outcome::Finished);
  assert(!m.step(Batch::Finished, 90000, 1500));
}

void failedBatchReportsFailure() {
  Machine m;
  m.start();
  assert(!m.step(Batch::Failed, 1000, 1000) && m.outcome() == Outcome::Failed);
  assert(m.step(Batch::Failed, 1000 + transfer_policy::RECEIPT_MS, 1000) && m.outcome() == Outcome::Failed);
}

void silentSenderEndsTheBatch() {
  Machine m;
  m.start();
  assert(!m.step(Batch::Pending, 1000 + transfer_policy::IDLE_MS - 1, 1000) && m.outcome() == Outcome::None);
  // A request restarts the wait.
  assert(!m.step(Batch::Pending, 1000 + transfer_policy::IDLE_MS, 2000) && m.outcome() == Outcome::None);
  assert(!m.step(Batch::Pending, 2000 + transfer_policy::IDLE_MS, 2000) && m.outcome() == Outcome::Failed);
}

void aLaterResultCannotChangeTheOutcome() {
  Machine m;
  m.start();
  assert(!m.step(Batch::Finished, 1000, 1000));
  assert(!m.step(Batch::Failed, 1001, 1000) && m.outcome() == Outcome::Finished);
}

void returningToSettingsStopsWithoutAnOutcome() {
  Machine m;
  m.start();
  m.stop();
  assert(!m.running() && !m.step(Batch::Failed, 90000, 1000) && m.outcome() == Outcome::None);
}

void clockWrapIsHarmless() {
  Machine m;
  m.start();
  assert(!m.step(Batch::Pending, 100, UINT32_MAX - 100) && m.outcome() == Outcome::None);
}

void openingABookRestartsTheWaitForTheSender() {
  Machine m;
  m.start();
  m.hold(500);  // Nothing to restart while the batch runs.
  assert(!m.step(Batch::Finished, 1000, 1000) && m.outcome() == Outcome::Finished);
  m.hold(1000 + transfer_policy::RECEIPT_MS + 5000);
  assert(!m.step(Batch::Finished, 1000 + transfer_policy::RECEIPT_MS + 5000, 1000) && m.running());
  assert(m.step(Batch::Finished, 1000 + 2 * transfer_policy::RECEIPT_MS + 5000, 1000) && !m.running());
  m.hold(90000);  // Nor after the end.
  assert(!m.running());
}
}  // namespace

int main() {
  finishedBatchWaitsForTheSenderThenStops();
  failedBatchReportsFailure();
  silentSenderEndsTheBatch();
  aLaterResultCannotChangeTheOutcome();
  returningToSettingsStopsWithoutAnOutcome();
  clockWrapIsHarmless();
  openingABookRestartsTheWaitForTheSender();
  std::puts("transfer_policy: ok");
}
