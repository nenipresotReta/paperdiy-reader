#pragma once
#include <cstdint>
#include "auto_off.h"
void updatePowerIndicator(bool force=false);
bool chargingNow();
// M5PM1 PWR_SRC (0x04), read at most once per second: bit 0 5VIN, bit 1
// 5VINOUT, bit 2 battery node. known=false after a failed read.
uint8_t powerSourceBits(bool& known);

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
// Full speed for work that is not activity for automatic power-off, such as a
// status-area refresh.
void powerStatusWork();
// Battery-only automatic power-off. ready: no foreground work, transfer or open
// radio mode. On a trigger the caller shuts down; autoOffFailed() delays a retry
// after a shutdown that returned. The level is the last fuel-gauge sample.
auto_off::Trigger autoOffCheck(bool ready, uint32_t timeoutMs);
void autoOffFailed();
int autoOffBatteryLevel();
uint8_t rawPowerKeys();
void idlePower(bool stableScene, bool inputIdle, int stableCardLevel, bool radioActive=false, bool transferring=false);
const IdlePowerStats& idlePowerStats();
void logIdlePowerStatus();
#ifdef PAPERDIY_DIAGNOSTICS
// Explicit diagnostic only: exercise the production sleep/restore guard while
// USB is attached. Normal idlePower() continues to block all USB-host sleep.
bool powerDiagnosticSleep(uint32_t durationMs);
#endif
