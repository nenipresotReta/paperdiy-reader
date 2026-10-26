#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace lan_service {
// SD lifecycle runs even while the radio is off, to recover the one local Push
// and deliver completed publication notifications on the application task.
void storagePoll();
void storageStop();
void storageChanged();
bool storageBusy();
bool pushReady();
bool takeLibraryChanged();
// live=true is P: paired senders replace Push without a batch prompt, and
// batch offers, pairing and file uploads are refused until the next start().
bool start(const char* address,bool live=false);
void stop();
bool poll();
bool active();
// Reconcile a recovered Push row after the caller's ordinary SD scan.
bool refreshLibrary();
bool isPushPath(const char* path);
const char* currentPushPath();
// Set when a different Push body was published; cleared by reading it.
bool takePushUpdated();
// The current Push asked an open reader to keep its position (else page 1).
bool pushKeepsPosition();

// Consent prompt for one offered batch. Only a short A+B on the Settings N
// panel accepts it; switching off N, leaving Settings or 60 s cancels it.
struct Prompt {
  bool active=false,firstContact=false,hasPush=false;
  std::string label,sas,pushTitle,replacesPush;
  std::vector<std::string> names;
  size_t total=0;
};
bool promptPending();
const Prompt& prompt();
bool acceptPrompt();

// Sender management for the AP WebUI. Refuses while the LAN receiver runs.
enum class RegistryStatus:uint8_t {Ok,Busy,Unavailable,Unreadable,Invalid};
struct ClientInfo {std::string clientId,label;};
RegistryStatus listClients(std::vector<ClientInfo>& out);
RegistryStatus revokeClient(const std::string& clientId);
RegistryStatus forgetClients();
// Unlike the AP WebUI calls, this also answers while the receiver runs.
RegistryStatus pairedCount(size_t& count);
#ifdef PAPERDIY_DIAGNOSTICS
void printStatus();
#endif
}
