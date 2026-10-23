#pragma once
#include <cstddef>
bool prepareMarkdown(const char* source,size_t bytes);
void clearMarkdown();
size_t layoutMarkdown(size_t sourceOffset,bool draw,int top=24);
