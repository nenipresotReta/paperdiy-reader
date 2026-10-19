#pragma once
#include <Arduino.h>
#include <vector>
extern bool sdOK;
extern std::vector<String> sdBooks;
extern size_t selectedBook;
extern String sdError;
bool scanSD();
bool openSD(size_t index,bool paginate=true);
