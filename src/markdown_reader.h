#pragma once
#include <cstddef>
namespace reader_work {class Budget;}
bool prepareMarkdown(const char* source,size_t bytes);
void clearMarkdown();
size_t layoutMarkdown(size_t sourceOffset,bool draw,int top=24,reader_work::Budget* budget=nullptr);
