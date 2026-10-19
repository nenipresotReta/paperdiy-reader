#pragma once
#include <Arduino.h>
void showLibrary(bool sceneChange=true);
void updateLibrarySelection(size_t oldSelection);
void showBook(bool sceneChange=false);
void showLoading();
void showError(const String& message);
void updateLibraryBattery();
void showShutdownOverlay();
