// Browser-only memory adapter around the production EPUB importer and PDT1 codec.
// It owns one bounded job. The worker is terminated to cancel synchronous work.
#include "epub_core.h"
#include "epub_document.h"
#include "prepared_epub.h"
#include "portable_sha256.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace {
using Buffer = std::unique_ptr<uint8_t, decltype(&std::free)>;
Buffer source(nullptr, std::free), result(nullptr, std::free);
size_t sourceBytes=0, resultBytes=0;
const char* error="";
int fail(const char* message) { error=message; result.reset(); resultBytes=0; return 1; }

int prepare(bool flat) {
  result.reset();resultBytes=0;error="";
  if (!source || sourceBytes < 22) return fail("No EPUB source");
  prepared_epub::Header header;header.sourceBytes=sourceBytes;
  portable_sha256::Hash sourceHash;sourceHash.update(source.get(),sourceBytes);sourceHash.finish(header.sourceDigest);
  Buffer text(static_cast<uint8_t*>(std::malloc(prepared_epub::MAX_TEXT_BYTES+1)),std::free);
  if(!text)return fail("Not enough memory for EPUB text");
  epub::Importer importer;epub::structure::Collector structure;
  importer.fileSize=uint32_t(sourceBytes);importer.output=reinterpret_cast<char*>(text.get());
  importer.capacity=prepared_epub::MAX_TEXT_BYTES;importer.alloc=std::malloc;
  importer.structure=flat?nullptr:&structure;
  importer.read=[](uint32_t offset,void* out,size_t bytes)->size_t {
    if(offset>sourceBytes || bytes>sourceBytes-offset)return 0;
    std::memcpy(out,source.get()+offset,bytes);return bytes;
  };
  if(!importer.run())return fail("Unsupported, damaged, or oversized EPUB");
  // The importer owns no borrowed archive bytes after run(). Release the largest
  // allocation before packing; canonical text and semantic offsets remain fixed.
  source.reset();sourceBytes=0;
  header.textBytes=uint32_t(importer.used);header.flags=uint32_t(importer.language);
  std::vector<uint8_t> metadata;
  epub_document::Document semantic;
  if(structure.ready() && semantic.assign(structure,reinterpret_cast<char*>(text.get()),importer.used)) {
    try { if(!semantic.encode(metadata))metadata.clear(); }
    catch(const std::bad_alloc&) {metadata.clear();}
  }
  if(metadata.size()>prepared_epub::MAX_METADATA_BYTES)metadata.clear();
  if(metadata.empty() && importer.unsafeFlatLanguage)header.flags=uint32_t(text_language::Language::None);
  header.metadataBytes=uint32_t(metadata.size());
  epub_document::Document checked;
  if(!metadata.empty() && !checked.decode(metadata.data(),metadata.size(),reinterpret_cast<char*>(text.get()),importer.used))
    return fail("Not enough memory to validate EPUB structure");
  portable_sha256::Hash payloadHash;
  payloadHash.update(text.get(),importer.used);
  if(!metadata.empty())payloadHash.update(metadata.data(),metadata.size());
  payloadHash.finish(header.payloadDigest);
  if(!prepared_epub::valid(header))return fail("Invalid prepared EPUB");
  const size_t bytes=size_t(prepared_epub::fileBytes(header));
  Buffer packed(static_cast<uint8_t*>(std::malloc(bytes)),std::free);
  if(!packed)return fail("Not enough memory to pack EPUB");
  if(!prepared_epub::encode(header,packed.get(),prepared_epub::HEADER_BYTES))return fail("Invalid prepared header");
  std::memcpy(packed.get()+prepared_epub::HEADER_BYTES,text.get(),importer.used);
  if(!metadata.empty())std::memcpy(packed.get()+prepared_epub::HEADER_BYTES+importer.used,metadata.data(),metadata.size());
  result=std::move(packed);resultBytes=bytes;return 0;
}
}

extern "C" {
void pdt_release() {source.reset();result.reset();sourceBytes=0;resultBytes=0;error="";}
uint8_t* pdt_allocate(uint32_t bytes) {
  pdt_release();
  if(bytes<22 || bytes>prepared_epub::MAX_SOURCE_BYTES){error="EPUB must be between 22 bytes and 64 MiB";return nullptr;}
  source.reset(static_cast<uint8_t*>(std::malloc(bytes)));
  if(!source){error="Not enough memory for EPUB source";return nullptr;}
  sourceBytes=bytes;return source.get();
}
int pdt_prepare(int flat) {
  if(flat!=0 && flat!=1)return fail("Invalid preparation option");
  try {return prepare(flat!=0);}
  catch(const std::bad_alloc&) {return fail("Not enough memory to prepare EPUB");}
  catch(...) {return fail("EPUB preparation failed");}
}
const uint8_t* pdt_result() {return result.get();}
uint32_t pdt_result_size() {return uint32_t(resultBytes);}
const char* pdt_error() {return error;}
}
