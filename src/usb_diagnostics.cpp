#include "usb_diagnostics.h"
#ifdef ARDUINO
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif
namespace usb_storage {
namespace {
#ifdef ARDUINO
RTC_NOINIT_ATTR diagnostic_detail::Retained retained;
portMUX_TYPE diagnosticMux=portMUX_INITIALIZER_UNLOCKED;
struct Guard {Guard(){portENTER_CRITICAL(&diagnosticMux);}~Guard(){portEXIT_CRITICAL(&diagnosticMux);}};
uint32_t resetReason(){return uint32_t(esp_reset_reason());}
#else
diagnostic_detail::Retained retained{};
struct Guard {};
uint32_t resetReason(){return 0;}
#endif
Diagnostics view;
bool initialized=false;
void initialize(){
  if(initialized)return;
  view.resetReason=resetReason();
  const int previous=diagnostic_detail::newest(retained);view.previousValid=previous>=0;
  if(view.previousValid)view.previous=retained.slots[previous].record;
  view.current={};view.current.stage=uint32_t(DiagnosticStage::Boot);
  diagnostic_detail::publish(retained,view.current);initialized=true;
}
}
void initializeDiagnostics(){Guard guard;initialize();}
Diagnostics diagnostics(){Guard guard;initialize();return view;}
void diagnosticMark(DiagnosticStage stage,uint32_t lba,uint32_t offset,uint32_t length,uint32_t stack){
  Guard guard;initialize();diagnostic_detail::mark(view.current,stage,lba,offset,length,stack);
  diagnostic_detail::publish(retained,view.current);
}
const char* diagnosticStageName(DiagnosticStage stage){
  switch(stage){
    case DiagnosticStage::Boot:return "boot";case DiagnosticStage::Prepare:return "prepare";
    case DiagnosticStage::RawInitialize:return "raw_init";case DiagnosticStage::HostOwned:return "host_owned";
    case DiagnosticStage::Read:return "read";case DiagnosticStage::Write:return "write";
    case DiagnosticStage::Sync:return "sync";case DiagnosticStage::Yield:return "yield";
    case DiagnosticStage::Eject:return "eject";case DiagnosticStage::Release:return "release";
    case DiagnosticStage::Inactive:return "inactive";default:return "none";
  }
}
}
