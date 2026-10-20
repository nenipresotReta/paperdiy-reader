#include <fstream>
#include <filesystem>
#include <iterator>
#include <iostream>
#include "epub_core.h"
int main(int argc,char** argv){
 if(argc<2||argc>3)return 2;
 std::string mode=argc==3?argv[2]:"";
 size_t allocation=0,failAllocation=mode.rfind("--fail-alloc=",0)==0?std::stoul(mode.substr(13)):0;
 std::ifstream file;if(!std::filesystem::is_directory(argv[1]))file.open(argv[1],std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{}),out(4*1024*1024+1);
 epub::Importer e;e.fileSize=bytes.size();e.output=out.data();e.capacity=out.size()-1;e.alloc=[&](size_t n)->void*{return ++allocation==failAllocation?nullptr:malloc(n);};
 e.xmlMemoryAvailable=[&](size_t required,size_t largest){return mode!="--deny-xml-memory"&&required>=largest&&largest>=4096;};
 e.read=[&](uint32_t at,void* p,size_t n)->size_t{if(at>bytes.size()||n>bytes.size()-at)return 0;memcpy(p,bytes.data()+at,n);return n;};
 if(std::filesystem::is_directory(argv[1])){
  std::filesystem::path root(argv[1]);
  e.resourceSize=[root](const std::string& name)->size_t{auto p=root/name;return std::filesystem::is_regular_file(p)?std::filesystem::file_size(p):SIZE_MAX;};
  e.resourceRead=[root](const std::string& name,void* data,size_t n)->bool{std::ifstream f(root/name,std::ios::binary);return bool(f.read((char*)data,n));};
 }
 if(!e.run()){std::cerr<<e.error<<'\n';return 1;}std::cout.write(out.data(),e.used);return 0;
}
