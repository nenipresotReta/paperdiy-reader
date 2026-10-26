#include "console.h"
#include <Arduino.h>
#include <M5Unified.h>
#include "usb_transport.h"
#include "usb_storage.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include "power.h"
#include "idle_policy.h"
#include "card_sleep.h"
#include "storage_validation.h"

namespace {
idle_power::Policy idlePolicy;
IdlePowerStats stats;
uint32_t lastSourceCheck = 0;
bool sourceChecked = false;

struct CardSleepBackend {
  bool disableInterrupt(){return gpio_intr_disable(GPIO_NUM_48)==ESP_OK;}
  bool armWake(int level){return gpio_wakeup_enable(GPIO_NUM_48,level==LOW?GPIO_INTR_HIGH_LEVEL:GPIO_INTR_LOW_LEVEL)==ESP_OK;}
  bool disarmWake(){return gpio_wakeup_disable(GPIO_NUM_48)==ESP_OK;}
  bool restoreEdge(){return gpio_set_intr_type(GPIO_NUM_48,GPIO_INTR_ANYEDGE)==ESP_OK;}
  bool enableInterrupt(){return gpio_intr_enable(GPIO_NUM_48)==ESP_OK;}
  int level(){return digitalRead(48);}
  void revokeProofs(){storage_validation::sourceChanged();}
  void revokeTrust(){storage_validation::invalidate();}
  void signalChange(){storage_validation::signalCardEvent();}
};

void cpuFrequency(uint32_t mhz) {
  if (getCpuFrequencyMhz() == mhz) return;
  if (setCpuFrequencyMhz(mhz)) ++stats.frequencyChanges;
  else ++stats.frequencyFailures;
}

void checkPowerSource() {
  stats.usbHost = usb_transport::blocksSleep();
  uint32_t now = millis();
  if (sourceChecked && uint32_t(now - lastSourceCheck) < 1000) return;
  sourceChecked = true;
  lastSourceCheck = now;
  uint8_t source = 0;
  // Unlike getPowerSource(), this distinguishes an I2C failure from no input.
  stats.sourceKnown = M5.Power.M5pm1.readRegister(0x04, &source, 1);
  stats.powerSource = source;
}

void clearWakeSources() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  gpio_wakeup_disable(GPIO_NUM_3);
  gpio_wakeup_disable(GPIO_NUM_4);
  // The card guard disables its wake mode and restores the awake edge ISR.
}

esp_err_t prepareWakeSources(uint32_t sleepMs) {
  esp_err_t result = gpio_wakeup_enable(GPIO_NUM_4, GPIO_INTR_LOW_LEVEL);
  if (result == ESP_OK)
    result = gpio_wakeup_enable(GPIO_NUM_3, GPIO_INTR_LOW_LEVEL);
  if (result == ESP_OK) result = esp_sleep_enable_gpio_wakeup();
  if (result == ESP_OK)
    result = esp_sleep_enable_timer_wakeup(uint64_t(sleepMs) * 1000);
  return result;
}

struct SleepResult {
  esp_err_t error=ESP_OK;
  bool cancelled=false,slept=false;
  uint64_t elapsedUs=0;
  esp_sleep_wakeup_cause_t cause=ESP_SLEEP_WAKEUP_UNDEFINED;
};
SleepResult lightSleepWindow(int cardLevel,uint32_t sleepMs,bool allowUsb=false){
  CardSleepBackend backend;card_sleep::Window<CardSleepBackend> card(backend,cardLevel);
  SleepResult result;
  result.error=card.begin()?prepareWakeSources(sleepMs):ESP_FAIL;
  // Keep the first wake key as normal input; only abandon entering sleep.
  result.cancelled=rawPowerKeys()||digitalRead(48)!=cardLevel||(!allowUsb&&usb_transport::blocksSleep());
  if(result.error==ESP_OK&&!result.cancelled){
    const uint64_t started=esp_timer_get_time();
    result.error=esp_light_sleep_start();
    if(result.error==ESP_OK){
      result.slept=true;result.elapsedUs=uint64_t(esp_timer_get_time())-started;
      result.cause=esp_sleep_get_wakeup_cause();
    }
  }
  clearWakeSources();
  if(!card.finish())result.error=ESP_FAIL;
  return result;
}
} // namespace

uint8_t rawPowerKeys() {
  return (digitalRead(4) == LOW ? 1 : 0) | (digitalRead(3) == LOW ? 2 : 0);
}

void powerActivity() {
  cpuFrequency(240);
  idlePolicy.noteActivity(millis());
}

const IdlePowerStats& idlePowerStats() { return stats; }

void logIdlePowerStatus() {
  Console.printf("IDLE_POWER cpu_mhz=%u switches=%u freq_failures=%u sleeps=%u "
                "sleep_ms=%llu timer=%u gpio=%u failures=%u wake_keys=%u "
                "source_known=%d source=%u usb=%d\n",
      getCpuFrequencyMhz(), stats.frequencyChanges, stats.frequencyFailures,
      stats.sleeps, (unsigned long long)(stats.sleepUs / 1000),
      stats.timerWakes, stats.gpioWakes, stats.sleepFailures, stats.lastWakeKeys,
      stats.sourceKnown, stats.powerSource, stats.usbHost);
}

void idlePower(bool stableScene, bool inputIdle, int stableCardLevel, bool radioActive) {
  checkPowerSource();
  // Conservatively keep native USB awake on all external power, including a
  // suspended host or a charging-only cable. Failed PMIC reads also block sleep.
  bool sleepBlocked = stats.usbHost || !stats.sourceKnown ||
      (stats.powerSource & 3) || !(stats.powerSource & 4);
  bool cardStable = digitalRead(48) == stableCardLevel;
  auto plan = idlePolicy.plan(millis(),
      {stableScene && cardStable, inputIdle, rawPowerKeys() != 0, sleepBlocked, radioActive,usb_storage::hostOwnsCard()});
  if (!plan.idle) {
    cpuFrequency(240);
    delay(10);
    return;
  }

  // waitDisplay is the completion barrier; displayBusy only reports queue
  // capacity in this panel driver. Its worker has already disabled BST_EN.
  M5.Display.waitDisplay();
  cpuFrequency(80);
  if (!plan.sleep) {
    delay(10);
    return;
  }

  const auto result=lightSleepWindow(stableCardLevel,plan.sleepMs);
  if (result.slept) {
    stats.sleepUs += result.elapsedUs;
    ++stats.sleeps;
    if (result.cause == ESP_SLEEP_WAKEUP_GPIO) ++stats.gpioWakes;
    if (result.cause == ESP_SLEEP_WAKEUP_TIMER) ++stats.timerWakes;
    stats.lastWakeKeys = rawPowerKeys();
  }
  if (result.error!=ESP_OK) {
    ++stats.sleepFailures;
    idlePolicy.noteSleepFailure(millis());
  }
  // Keep PSRAM, SD and the EPD controller powered; no remount or display init.
  // Do not call Display.wakeup(): it would leave the panel boost enabled.
  delay(1);
}

#ifdef PAPERDIY_DIAGNOSTICS
bool powerDiagnosticSleep(uint32_t durationMs){
  if(!durationMs||durationMs>1000)return false;
  M5.Display.waitDisplay();
  const auto result=lightSleepWindow(digitalRead(48),durationMs,true);
  return result.error==ESP_OK&&result.slept;
}
#endif

uint8_t powerSourceBits(bool& known){
  checkPowerSource();known=stats.sourceKnown;return stats.powerSource;
}
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
    Console.printf("POWER charging=%d led=%d applied=%d\n",next,next,ok);
  }
  charging=next;
}
