#pragma once
bool beginRefresh(bool sceneChange=false,bool reading=false);
void endRefresh(const char* reason,int x=0,int y=0,int w=0,int h=0);
void displayInit();
void displaySetRotated(bool rotated);
void logDisplayStatus();
