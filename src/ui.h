#pragma once
#include <Arduino.h>
extern bool settingsSelected,settingsNetwork,settingsRotated;
void showLibrary(bool sceneChange=true);
void updateLibrarySelection(size_t oldSelection,bool oldSettings=false);
void showBook(bool sceneChange=false);
void showSettings(bool sceneChange=true);
void updateSettingsNetwork();
void showLoading();
void showError(const String& message);
void updateLibraryBattery();
void saveBatteryHistory();
void showShutdownOverlay();
