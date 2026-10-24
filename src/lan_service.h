#pragma once
#include <cstdint>
namespace lan_service {
// SD lifecycle runs even while the radio is off, to recover the one local Push
// and deliver completed publication notifications on the application task.
void storagePoll();
void storageStop();
void storageChanged();
bool storageBusy();
bool pushReady();
bool takeLibraryChanged();
bool start(const char* address);
void stop();
bool poll();
bool active();
// Reconcile a recovered Push row after the caller's ordinary SD scan.
bool refreshLibrary();
bool isPushPath(const char* path);
const char* currentPushPath();
#ifdef PAPERDIY_DIAGNOSTICS
void printStatus(bool pairing=false);
#endif
}
