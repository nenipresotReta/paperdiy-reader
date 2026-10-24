#pragma once
#include <Arduino.h>
#include "settings_registry.h"
#include "usb_features.h"

enum class NetworkMode : uint8_t {Off,Sta,Ap,Usb};

inline NetworkMode nextNetworkMode(NetworkMode mode){
  switch(mode){
    case NetworkMode::Off:return NetworkMode::Sta;
    case NetworkMode::Sta:return NetworkMode::Ap;
    case NetworkMode::Ap:return PAPERDIY_EXPERIMENTAL_USB_MSC?NetworkMode::Usb:NetworkMode::Off;
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
extern String settingsUsbStatus;
extern String settingsLanType,settingsLanName,settingsLanState,settingsLanPairing;
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
void updateLibraryBattery();
void saveBatteryHistory();
void showShutdownOverlay();
