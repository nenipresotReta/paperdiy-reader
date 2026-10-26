#pragma once
#include <Arduino.h>
#include "settings_registry.h"

// Values appear in STATUS/LIVE_STATUS output. 3 was USB storage (removed);
// Live keeps 4 so logs stay comparable across versions.
enum class NetworkMode : uint8_t {Off,Sta,Ap,Live=4};

inline NetworkMode nextNetworkMode(NetworkMode mode){
  switch(mode){
    case NetworkMode::Off:return NetworkMode::Sta;
    case NetworkMode::Sta:return NetworkMode::Ap;
    case NetworkMode::Ap:return NetworkMode::Live;
    default:return NetworkMode::Off;
  }
}

extern bool settingsSelected,settingsRotated;
extern SettingsId settingsId;
extern NetworkMode networkMode;
extern String settingsApSsid,settingsApPassword,settingsApUrl;
extern bool settingsApClientConnected;
extern String settingsStaSsid,settingsStaIp,settingsStaError;
extern bool settingsStaConfigured,settingsStaConnected;
extern String settingsLanType,settingsLanName,settingsLanState;
// P: Settings status line, and the icon at the bottom-left of the library and
// the reader while P is selected: online, connecting/retrying, or paused/stopped.
enum class LiveIcon : uint8_t {None,Online,Connecting,Off};
extern String settingsLiveStatus;
extern LiveIcon liveIcon;
void updateSettingsDelivery();
void showLibrary(bool sceneChange=true);
void updateLibrarySelection(size_t oldSelection,bool oldSettings=false,SettingsId oldId=SettingsId::Network);
void showBook(bool sceneChange=false);
void updateBookFooter();
void showSettings(bool sceneChange=true);
void updateSettingsMode();
void updateSettingsNetwork();
void showLoading();
void showError(const String& message);
void showPrefetchFailureBanner();
void updateLibraryBattery();
void updateLibraryLive();
void saveBatteryHistory();
void showShutdownOverlay();
