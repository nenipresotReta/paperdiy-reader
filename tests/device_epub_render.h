#pragma once
#include "console.h"
// Opt-in synthetic rendering acceptance. No SD/NVS reads or writes, no change
// to source anchors, page offsets, selected book or user text. The caller only
// admits this while no book is loaded, and restores its scene after checks.
#include "epub_core.h"
#include "epub_reader.h"
#include "reader.h"
#include "content_source.h"
#include <memory>
#include <string>
#include <utility>

namespace epub_device_test {
inline bool run(unsigned fixture,bool preview){
  if(bookLength||reader_content::active()){
    Console.println("DEVICE_EPUB_REJECT loaded_book=1");return false;
  }
  const std::string container="<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>";
  const std::string package="<package><manifest><item id='c' href='c.xhtml'/></manifest><spine><itemref idref='c'/></spine></package>";
  std::string chapter;
  if(fixture==0){
    chapter="<html><body style='writing-mode:vertical-rl' lang='ja'><p>日本語の縦書き。<ruby>今日<rt>きょう</rt></ruby>は<span style='text-combine-upright:all'>24</span>日。</p><p><ruby>文字<rt>gjpqy</rt></ruby>、髙い山と𠮷。gjpqyW|</p>";
    for(unsigned i=0;i<25;++i)chapter+="<p>雨にも負けず、風にも負けず。ABC（日本語）「縦書き」の句読点。</p>";
  }else{
    chapter="<html><body><p>MathML and tables</p><math display='block'><mfrac><msup><mi>x</mi><mn>2</mn></msup><msqrt><mi>y</mi></msqrt></mfrac></math><math display='block'><msqrt><msqrt><msqrt><msqrt><mi>x</mi></msqrt></msqrt></msqrt></msqrt></math><table><tr><th>Item</th><th>Value</th></tr>";
    for(unsigned i=0;i<50;++i)chapter+="<tr><td>Row "+std::to_string(i+1)+" 日本語</td><td>Bounded native text</td></tr>";
    chapter+="</table>";
  }
  chapter+="</body></html>";
  std::unique_ptr<char,decltype(&free)> text(static_cast<char*>(ps_malloc(32768)),free);
  if(!text)return false;
  epub::structure::Collector collector;epub::Importer importer;
  importer.output=text.get();importer.capacity=32767;importer.structure=&collector;
  auto resource=[&](const std::string& path)->const std::string*{
    if(path=="META-INF/container.xml")return &container;
    if(path=="book.opf")return &package;
    if(path=="c.xhtml")return &chapter;return nullptr;
  };
  importer.resourceSize=[&](const std::string& path){const auto* value=resource(path);return value?value->size():SIZE_MAX;};
  importer.resourceRead=[&](const std::string& path,void* out,size_t size){const auto* value=resource(path);if(!value||value->size()!=size)return false;std::memcpy(out,value->data(),size);return true;};
  importer.alloc=[](size_t size){return ps_malloc(size);};importer.tick=[](){delay(1);};
  auto previous=std::move(bookDocument);
  bool ok=importer.run()&&bookDocument.assign(collector,text.get(),importer.used);
  const size_t maths=bookDocument.mathCount(),tables=bookDocument.tableCount(),rubies=bookDocument.rubyCount();
  ok=ok&&(fixture==0?(bookDocument.hasVertical()&&rubies==2):(maths==2&&tables==1));
  size_t at=0;unsigned pages=0;
  prepareReaderFont();
  while(ok&&at<importer.used&&pages<32){
    const size_t measured=layoutEpub(at,false);
    M5.Display.waitDisplay();M5.Display.setAutoDisplay(false);M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.startWrite();M5.Display.clearClipRect();M5.Display.fillScreen(TFT_WHITE);
    const size_t drawn=layoutEpub(at,true);
    M5.Display.clearClipRect();M5.Display.endWrite();
    ok=drawn==measured&&drawn>at&&drawn<=importer.used;
    ++pages;at=drawn;
    if(preview){M5.Display.display();M5.Display.waitDisplay();break;}
  }
  ok=ok&&(preview||at==importer.used)&&uxTaskGetStackHighWaterMark(nullptr)>=4096;
  bookDocument=std::move(previous);prepareEpubMetrics();
  Console.printf("DEVICE_EPUB_DONE fixture=%u preview=%u ok=%u pages=%u rubies=%u maths=%u tables=%u stack_free=%u\n",
    fixture,unsigned(preview),unsigned(ok),pages,unsigned(rubies),unsigned(maths),unsigned(tables),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  return ok;
}
}
