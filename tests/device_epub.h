#pragma once
#include "console.h"
// Opt-in hardware regression: in-memory synthetic books, no SD or NVS writes.
#include "epub_core.h"
#include <esp_heap_caps.h>
inline void deviceEpubChecks(){
  unsigned passed=0;
  auto check=[&](const char* name,const std::string& chapter,const char* expectedError){
    const std::string container="<container><rootfiles><rootfile full-path=\"book.opf\"/></rootfiles></container>";
    const std::string package="<package><manifest><item id=\"c\" href=\"chapter.xhtml\"/></manifest><spine><itemref idref=\"c\"/></spine></package>";
    char output[64];epub::Importer importer;importer.output=output;importer.capacity=sizeof(output)-1;
    auto resource=[&](const std::string& path)->const std::string*{
      if(path=="META-INF/container.xml")return &container;
      if(path=="book.opf")return &package;
      if(path=="chapter.xhtml")return &chapter;
      return nullptr;
    };
    importer.resourceSize=[&](const std::string& path){auto s=resource(path);return s?s->size():SIZE_MAX;};
    importer.resourceRead=[&](const std::string& path,void* p,size_t n){auto s=resource(path);if(!s||n!=s->size())return false;memcpy(p,s->data(),n);return true;};
    importer.alloc=[](size_t n){return ps_malloc(n);};importer.tick=[](){delay(1);};
    importer.xmlMemoryAvailable=[](size_t total,size_t largest){return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=total&&heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=largest;};
    bool ok=importer.run();bool success=expectedError?(!ok&&importer.error==expectedError):(ok&&std::string(output,importer.used)=="x\n\n");
    if(uxTaskGetStackHighWaterMark(nullptr)<4096)success=false;
    if(success)++passed;
    Console.printf("DEVICE_XML_TEST name=%s passed=%d stack_free=%u error=%s\n",name,success,unsigned(uxTaskGetStackHighWaterMark(nullptr)),importer.error.c_str());
  };
  for(unsigned nested:{30u,31u}){
    std::string chapter="<html><body>";
    for(unsigned i=0;i<nested;++i)chapter+="<div>";
    chapter+='x';for(unsigned i=0;i<nested;++i)chapter+="</div>";
    chapter+="</body></html>";
    check(nested==30?"depth_boundary":"depth_limit",chapter,nested==30?nullptr:"XML depth exceeds 32 levels");
  }
  std::string dense="<html><body><p>";
  for(unsigned i=0;i<20000;++i)dense+="<span>x</span>";
  dense+="</p></body></html>";
  check("node_budget",dense,"XML DOM resource budget exceeded");
  Console.printf("DEVICE_XML_DONE passed=%u total=3\n",passed);
}
