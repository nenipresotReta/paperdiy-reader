#pragma once
// indicator: a status area (P icon, battery, loading icon, reader footer,
// delivery progress lines). It always uses the fast mode and neither counts
// toward nor triggers the periodic full refresh, so a status change never
// flashes the whole panel. Ghosting there is cleared by the next scene refresh.
bool beginRefresh(bool sceneChange=false,bool reading=false,bool indicator=false);
void endRefresh(const char* reason,int x=0,int y=0,int w=0,int h=0);
void displayInit();
void displaySetRotated(bool rotated);
void logDisplayStatus();
