#pragma once
#include <Arduino.h>
#include <vector>
extern bool sdOK;
extern std::vector<String> sdBooks;
extern size_t selectedBook;
extern String sdError;
bool scanSD();
bool openSD(size_t index,bool paginate=true,bool forceReflow=false);
#include "sd_open_state.h"
SDOpenResult beginOpenSD(size_t index,bool paginate=true,bool forceReflow=false);
SDOpenResult pollOpenSD();
void cancelOpenSD();
bool openSDPending();
bool sdOpenClosePending();
// Main-thread input probe only. It must not mutate UI, storage or progress.
void setSDOpenCancelProbe(bool (*probe)());
// Cooperative application-loop work; never call from an ISR or network task.
bool sdBackgroundPending();
void pollSDBackground();
void cancelSDBackground();

// Authenticated background fill of the current EPUB content source. It is
// armed when a cache window installs, starts only after the first reading
// paint, performs at most one 8 KiB block per application-loop step, and is
// bound to the book generation, buffer, length and original storage lease.
void contentPrefetchArm();
void contentPrefetchFirstPaint();
bool contentPrefetchPending();
bool contentPrefetchStep();
void cancelContentPrefetch();
bool contentPrefetchFailed();
const char* contentPrefetchStateName();
#ifdef PAPERDIY_DIAGNOSTICS
void logSDCacheInfo();
#endif

// New physical insertion resets the per-session SD speed fallback.
void resetSDSpeed();

// The caller must stop all other SD users before begin, and synchronize and
// uninitialize the USB raw backend before end. Local opens/scans are gated.
uint32_t sdStorageFrequency();
bool beginUsbStorageHandoff();
bool endUsbStorageHandoff();
bool sdOwnedByUsb();
