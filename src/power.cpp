#include <Arduino.h>
#include <M5Unified.h>
#include "power.h"
static bool charging=false,ledKnown=false;
static uint32_t lastCheck=0;
bool chargingNow(){return charging;}
void updatePowerIndicator(bool force){
  if(!force&&millis()-lastCheck<5000)return;lastCheck=millis();
  auto state=M5.Power.isCharging();
  if(state==m5::Power_Class::charge_unknown)return;
  bool next=state==m5::Power_Class::is_charging;
  if(force||!ledKnown||next!=charging){
    // PaperDIY schematic: PY_LED -> R21 -> LED1 -> GND, active high.
    // Only the indicator default is changed; CHG_EN and charge configuration are untouched.
    bool ok=M5.Power.M5pm1.setLedEnLevel(next);ledKnown=ok;
    Serial.printf("POWER charging=%d led=%d applied=%d\n",next,next,ok);
  }
  charging=next;
}
