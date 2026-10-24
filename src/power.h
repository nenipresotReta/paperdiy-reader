#pragma once
#include <cstdint>
void updatePowerIndicator(bool force=false);
bool chargingNow();

struct IdlePowerStats {
  uint64_t sleepUs = 0;
  uint32_t sleeps = 0, timerWakes = 0, gpioWakes = 0, sleepFailures = 0;
  uint32_t frequencyChanges = 0, frequencyFailures = 0;
  uint8_t lastWakeKeys = 0, powerSource = 0;
  bool sourceKnown = false, usbHost = false;
};

// All calls run on the application task. Foreground work restores full speed;
// periodic housekeeping must not reset the inactivity clock.
void powerActivity();
uint8_t rawPowerKeys();
void idlePower(bool stableScene, bool inputIdle, int stableCardLevel, bool radioActive=false);
const IdlePowerStats& idlePowerStats();
void logIdlePowerStatus();
#ifdef PAPERDIY_DIAGNOSTICS
// Explicit diagnostic only: exercise the production sleep/restore guard while
// USB is attached. Normal idlePower() continues to block all USB-host sleep.
bool powerDiagnosticSleep(uint32_t durationMs);
#endif
