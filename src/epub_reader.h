#pragma once
#include "epub_document.h"
namespace reader_work {class Budget;}
extern epub_document::Document bookDocument;
// A temporary memory shortage may flatten one open, but must not replace a
// richer persistent cache or become the resident view used on the next open.
extern bool bookEpubTransientFallback;
void prepareEpubMetrics();
size_t layoutEpub(size_t from,bool draw,int top=24,reader_work::Budget* budget=nullptr);
