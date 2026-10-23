#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#include <vector>
inline constexpr const char* FILE_WRITE="w";
inline constexpr const char* FILE_READ="r";
enum {CARD_NONE=0,CARD_SD=1};
struct MockNode {
 bool directory=false;
 uint64_t size=0;
 std::vector<uint8_t> bytes;
 unsigned openHandles=0;
};
struct MockSD;
class File {
 public:
  File()=default;
  File(MockSD* owner,const std::string& path,const std::shared_ptr<MockNode>& node);
  explicit operator bool()const{return node_&&open_;}
  bool isDirectory()const{return node_&&node_->directory;}
  const char* name()const{return name_.c_str();}
  const char* path()const{return path_.c_str();}
  uint64_t size()const{return node_?node_->size:0;}
  size_t write(const uint8_t* bytes,size_t count);
  size_t read(uint8_t* out,size_t count){if(!*this)return 0;count=std::min(count,node_->bytes.size()-std::min(readAt_,node_->bytes.size()));memcpy(out,node_->bytes.data()+readAt_,count);readAt_+=count;return count;}
  void flush(){}
  void close(){if(open_&&node_){--node_->openHandles;open_=false;}}
  File openNextFile();
 private:
  MockSD* owner_=nullptr;
  std::string path_,name_;
  std::shared_ptr<MockNode> node_;
  bool open_=false;
  size_t iterator_=0,readAt_=0;
  std::vector<std::string> children_;
};
struct MockSD {
 std::map<std::string,std::shared_ptr<MockNode>> files;
 bool mounted=true,failOpen=false,failMkdir=false,failRename=false,failWrite=false;
 uint64_t capacity=64ull*1024*1024*1024,used=0;
 unsigned renames=0,removes=0;
 uint8_t cardType()const{return mounted?CARD_SD:CARD_NONE;}
 uint64_t totalBytes()const{return mounted?capacity:0;}
 uint64_t usedBytes()const{return used;}
 bool exists(const String& path)const{return mounted&&files.count(path.c_str());}
 bool mkdir(const String& path){
   if(!mounted||failMkdir)return false;
   auto& node=files[path.c_str()];if(!node)node=std::make_shared<MockNode>();
   node->directory=true;return true;
 }
 File open(const String& path,const char* mode=FILE_READ){
   if(!mounted||failOpen)return {};
   auto found=files.find(path.c_str());
   if(found==files.end()){
     if(std::strcmp(mode,FILE_WRITE))return {};
     auto node=std::make_shared<MockNode>();files[path.c_str()]=node;
     return File(this,path.c_str(),node);
   }
   return File(this,path.c_str(),found->second);
 }
 bool remove(const String& path){
   ++removes;auto it=files.find(path.c_str());if(it==files.end())return false;
   used=used>=it->second->size?used-it->second->size:0;files.erase(it);return true;
 }
 bool rename(const String& from,const String& to){
   ++renames;if(!mounted||failRename||files.count(to.c_str()))return false;
   auto it=files.find(from.c_str());if(it==files.end()||it->second->openHandles)return false;
   files[to.c_str()]=it->second;files.erase(it);return true;
 }
 void add(const std::string& path,const std::string& content){
   auto node=std::make_shared<MockNode>();node->bytes={content.begin(),content.end()};node->size=content.size();files[path]=node;
 }
};
inline MockSD SD;
inline File::File(MockSD* owner,const std::string& path,const std::shared_ptr<MockNode>& node)
 :owner_(owner),path_(path),name_(path.substr(path.find_last_of('/')+1)),node_(node),open_(true){
 ++node_->openHandles;
 if(node_->directory)for(const auto& entry:owner_->files){
   if(entry.first.compare(0,path_.size()+1,path_+"/")==0&&entry.first.find('/',path_.size()+1)==std::string::npos)children_.push_back(entry.first);
 }
}
inline size_t File::write(const uint8_t* bytes,size_t count){
 if(!*this||!owner_->mounted||owner_->failWrite)return 0;
 if(count>owner_->capacity-owner_->used)return 0;
 // Retain small file contents; large size-limit cases need byte counts only.
 size_t retain=std::min(count,size_t(131072)-std::min(node_->bytes.size(),size_t(131072)));
 node_->bytes.insert(node_->bytes.end(),bytes,bytes+retain);
 node_->size+=count;owner_->used+=count;return count;
}
inline File File::openNextFile(){
 if(!isDirectory())return {};
 if(iterator_>=children_.size())return {};
 const auto path=children_[iterator_++];return File(owner_,path,owner_->files.at(path));
}
