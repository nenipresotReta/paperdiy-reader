#pragma once
// Opt-in hardware fixture; include only behind PAPERDIY_DEVICE_TESTS.
// Uses fixed in-flash text and the normal adapter. Never opens SD/NVS, replaces
// the user's book buffer, changes pageNumber/pageOffsets, or saves progress.
#include "markdown_samples.h"
#include "markdown_reader.h"
#include "math_layout.h"
#include "reader.h"
#include "ui.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace markdown_device_test {
inline void memory(const char* stage) {
  Serial.printf("DEVICE_MD_MEMORY stage=%s heap=%u psram=%u psram_largest=%u stack_free=%u arena=%u\n",
    stage,ESP.getFreeHeap(),ESP.getFreePsram(),
    unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
    unsigned(uxTaskGetStackHighWaterMark(nullptr)),unsigned(math_layout::MAX_SCRATCH_BYTES));
}
inline bool restoreAdapter() {
  clearMarkdown();
  const bool ok=!bookMarkdown || (book && bookLength && prepareMarkdown(book,bookLength));
  Serial.printf("DEVICE_MD_RESTORE adapter_ok=%d\n",ok);
  return ok;
}
inline void paintBegin() {
  M5.Display.waitDisplay();M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);M5.Display.startWrite();
  M5.Display.clearClipRect();M5.Display.fillScreen(TFT_WHITE);
  prepareReaderFont();M5.Display.setClipRect(20,20,520,896);
}
inline void paintEnd(bool refresh) {
  M5.Display.clearClipRect();M5.Display.endWrite();
  if(refresh){M5.Display.display();M5.Display.waitDisplay();}
}
inline void checks(void (*restoreScreen)()) {
  memory("before");
  const size_t bytes=sizeof(markdown_samples::mixed)-1;
  bool ok=prepareMarkdown(markdown_samples::mixed,bytes);
  memory("prepared");
  unsigned pages=0;size_t offset=0;
  while(ok && offset<bytes && pages<32) {
    const size_t measured=layoutMarkdown(offset,false);
    paintBegin();const size_t drawn=layoutMarkdown(offset,true);paintEnd(false);
    ok=measured==drawn && drawn>offset && drawn<=bytes &&
       (drawn==bytes || (uint8_t(markdown_samples::mixed[drawn])&0xc0)!=0x80);
    Serial.printf("DEVICE_MD_PAGE page=%u source=%u next=%u same=%d stack_free=%u\n",
      pages,unsigned(offset),unsigned(drawn),measured==drawn,unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    if(!ok)break;
    offset=drawn;++pages;
  }
  ok=ok && offset==bytes && pages>0 && uxTaskGetStackHighWaterMark(nullptr)>=2048;
  memory("rendered");
  const bool restored=restoreAdapter();ok=ok&&restored;
  if(restoreScreen)restoreScreen();else showSettings(true);
  memory("restored");
  Serial.printf("DEVICE_MD_DONE ok=%d pages=%u source_bytes=%u restored=%d\n",ok,pages,unsigned(bytes),restored);
}
// Synthetic prose only: no SD, NVS, book selection or progress mutation.
inline void benchmark() {
  constexpr size_t capacity=128u*1024;
  static const char paragraph[]="A careful reader preserves each word and the original source position. "
    "Representation and extraordinary navigation use bounded spaces. Final short line.\n\n";
  std::unique_ptr<char,decltype(&free)> sample(static_cast<char*>(ps_malloc(capacity+1)),free);
  if(!sample){Serial.println("DEVICE_MD_BENCH_DONE ok=0 allocation=1");return;}
  const size_t repeats=capacity/(sizeof(paragraph)-1),bytes=repeats*(sizeof(paragraph)-1);
  for(size_t i=0;i<repeats;++i)memcpy(sample.get()+i*(sizeof(paragraph)-1),paragraph,sizeof(paragraph)-1);
  sample.get()[bytes]=0;
  const uint64_t started=esp_timer_get_time();
  bool ok=prepareMarkdown(sample.get(),bytes);
  const uint64_t prepared=esp_timer_get_time();
  uint32_t previousHash=0;unsigned previousPages=0;
  for(unsigned pass=0;ok&&pass<2;++pass){
    size_t offset=0;unsigned pages=0;uint32_t hash=2166136261u;
    const uint64_t begin=esp_timer_get_time();
    while(offset<bytes&&pages<1024&&uint64_t(esp_timer_get_time())-started<20000000){
      const size_t next=layoutMarkdown(offset,false);
      if(next<=offset||next>bytes){ok=false;break;}
      hash=(hash^uint32_t(next))*16777619u;offset=next;++pages;delay(1);
    }
    ok=ok&&offset==bytes;
    if(pass)ok=ok&&hash==previousHash&&pages==previousPages;
    previousHash=hash;previousPages=pages;
    Serial.printf("DEVICE_MD_BENCH pass=%u bytes=%u pages=%u index_us=%llu prepare_us=%llu complete=%d stack_free=%u\n",
      pass,unsigned(bytes),pages,(unsigned long long)(esp_timer_get_time()-begin),
      (unsigned long long)(prepared-started),offset==bytes,unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
  clearMarkdown();sample.reset(); // Restore with the original PSRAM budget.
  ok=restoreAdapter()&&ok;
  Serial.printf("DEVICE_MD_BENCH_DONE ok=%d cpu_mhz=%u\n",ok,getCpuFrequencyMhz());
}
inline void preview(unsigned page) {
  const size_t bytes=sizeof(markdown_samples::mixed)-1;
  bool ok=prepareMarkdown(markdown_samples::mixed,bytes);size_t offset=0;
  for(unsigned i=0;ok&&i<page;++i){
    const size_t next=layoutMarkdown(offset,false);
    ok=next>offset&&next<bytes;offset=next;
  }
  size_t next=offset;
  if(ok){paintBegin();next=layoutMarkdown(offset,true);paintEnd(true);ok=next>offset&&next<=bytes;}
  memory("preview");
  const bool restored=restoreAdapter();
  Serial.printf("DEVICE_MD_PREVIEW page=%u source=%u next=%u ok=%d restored=%d\n",
    page,unsigned(offset),unsigned(next),ok&&restored,restored);
}
}  // namespace markdown_device_test

inline bool deviceMarkdownCommand(const String& command,void (*restoreScreen)()=nullptr) {
  if(command=="MD_BENCH"){markdown_device_test::benchmark();return true;}
  if(command=="MD_CHECKS"){markdown_device_test::checks(restoreScreen);return true;}
  if(command=="MD_PREVIEW 0"){markdown_device_test::preview(0);return true;}
  if(command=="MD_PREVIEW 1"){markdown_device_test::preview(1);return true;}
  if(command=="MD_RESTORE"){
    if(restoreScreen)restoreScreen();else showSettings(true);
    Serial.println("DEVICE_MD_SCREEN_RESTORED");return true;
  }
  return false;
}
