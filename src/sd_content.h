#pragma once
#include "content_source.h"
#include "content_manifest.h"
#include "storage_validation.h"
#include <algorithm>
#include <memory>

class SdContentSource:public reader_content::Source {
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  String path;
  char* text;
  content_manifest::Header header{};
  Buffer hashes{nullptr,free},scratch{nullptr,free};
  uint8_t loaded[content_manifest::MAX_BLOCKS/8]={};
  uint64_t leaseId=0;
  size_t first_=0,last_=0;
  bool ioFailed_=false;
  uint64_t readBytes_=0;
  bool has(size_t block)const{return loaded[block/8]&(1u<<(block%8));}
  bool failIO(){ioFailed_=true;storage_validation::invalidate();return false;}
 public:
  SdContentSource(const String& name,char* destination):path(name),text(destination){}
  const content_manifest::Header& manifest()const{return header;}
  bool ioFailed()const{return ioFailed_;}
  uint64_t readBytes()const{return readBytes_;}
  bool load(const reader_cache::Header& cache){
    File file=SD.open(path+".chunks","r");
    if(!file||file.size()<sizeof(header)||file.size()>content_manifest::MAX_BYTES)return false;
    const size_t got=file.read((uint8_t*)&header,sizeof(header));readBytes_+=got;
    if(got!=sizeof(header))return failIO();
    if(!content_manifest::valid(header,file.size(),cache))return false;
    hashes.reset((uint8_t*)ps_malloc(size_t(header.blocks)*32));scratch.reset((uint8_t*)ps_malloc(content_manifest::BLOCK_BYTES));
    if(!hashes||!scratch)return false;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)&header,offsetof(content_manifest::Header,digest));
    bool ok=true;
    for(size_t at=0;at<size_t(header.blocks)*32;){
      const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),size_t(header.blocks)*32-at);
      const size_t read=file.read(hashes.get()+at,count);readBytes_+=read;
      if(read!=count){failIO();ok=false;break;}
      mbedtls_sha256_update_ret(&sha,hashes.get()+at,count);at+=count;
    }
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);
    return ok&&!memcmp(digest,header.digest,32);
  }
  // Called only after the metadata/pages and source dependency graph match.
  void validateSource(){leaseId=storage_validation::lease();}
  size_t begin()const override{return first_;}
  size_t end()const override{return last_;}
  size_t length()const override{return header.textBytes;}
  void restoreSpan(size_t first,size_t last)override{
    if(first>last||last>header.textBytes)return;
    if(first<last)for(size_t block=first/content_manifest::BLOCK_BYTES;block<=(last-1)/content_manifest::BLOCK_BYTES;++block)if(!has(block))return;
    first_=first;last_=last;
  }
  bool pin(size_t first,size_t last)override{
    if(first>=last||last>header.textBytes||!hashes||!scratch)return false;
    const size_t firstBlock=first/content_manifest::BLOCK_BYTES?first/content_manifest::BLOCK_BYTES-1:0;
    const size_t lastBlock=std::min(size_t(header.blocks-1),(last-1)/content_manifest::BLOCK_BYTES+1);
    File file;size_t work=0;uint32_t yielded=millis();
    for(size_t block=firstBlock;block<=lastBlock;++block){
      if(has(block))continue;
      if(!leaseId||leaseId!=storage_validation::lease())return false;
      if(!file){file=SD.open(path,"r");if(!file)return failIO();}
      const size_t at=block*content_manifest::BLOCK_BYTES;
      const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),size_t(header.textBytes)-at);
      if(!file.seek(size_t(header.prefixBytes)+at))return failIO();
      const size_t got=file.read(scratch.get(),count);readBytes_+=got;
      if(got!=count)return failIO();
      uint8_t digest[32];mbedtls_sha256_ret(scratch.get(),count,digest,0);
      if(memcmp(digest,hashes.get()+block*32,32)||leaseId!=storage_validation::lease())return false;
      // Never overwrite a visible byte with an unverified read.
      memcpy(text+at,scratch.get(),count);loaded[block/8]|=uint8_t(1u<<(block%8));work+=count;
      if(work>=128u*1024||uint32_t(millis()-yielded)>=50){delay(1);work=0;yielded=millis();}
    }
    // Trim only boundary fragments belonging to neighboring unloaded blocks.
    // Every requested byte must remain inside the published complete-codepoint
    // span; UTF-8 validation never consults a byte outside loaded blocks.
    size_t begin=firstBlock*content_manifest::BLOCK_BYTES;
    const size_t end=std::min(size_t(header.textBytes),(lastBlock+1)*content_manifest::BLOCK_BYTES);
    if(begin){unsigned skipped=0;while(begin<end&&(uint8_t(text[begin])&0xc0)==0x80&&skipped<3){++begin;++skipped;}}
    size_t at=begin,validEnd=begin;
    while(at<end){
      const size_t start=at;const uint8_t c=uint8_t(text[at++]);unsigned n=0;uint32_t cp=0,min=0;
      if(c<128){if(!c)return false;validEnd=at;continue;}
      if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=128;}else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=2048;}else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=65536;}else return false;
      if(n>end-at){if(end!=header.textBytes){validEnd=start;break;}return false;}
      while(n--){const uint8_t b=uint8_t(text[at++]);if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
      if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;validEnd=at;
    }
    if(begin>first||validEnd<last)return false;
    first_=begin;last_=validEnd;return true;
  }
};
