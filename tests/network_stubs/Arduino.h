#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#define PROGMEM
inline constexpr int LOW=0,HIGH=1;
inline int testCardDetect=LOW;
inline int digitalRead(int){return testCardDetect;}

class String {
 public:
  String()=default;
  String(const char* value):value_(value?value:""){}
  String(const std::string& value):value_(value){}
  String(char value):value_(1,value){}
  template<class T,typename std::enable_if<std::is_integral<T>::value,int>::type=0>
  String(T value):value_(std::to_string(value)){}
  const char* c_str()const{return value_.c_str();}
  size_t length()const{return value_.length();}
  bool isEmpty()const{return value_.empty();}
  char charAt(size_t at)const{return at<value_.size()?value_[at]:0;}
  char operator[](size_t at)const{return charAt(at);}
  char& operator[](size_t at){return value_[at];}
  void reserve(size_t n){value_.reserve(n);}
  String substring(size_t from,size_t to)const{
    from=std::min(from,value_.size());to=std::min(to,value_.size());
    if(to<from)std::swap(from,to);
    return value_.substr(from,to-from);
  }
  String substring(size_t from)const{return substring(from,value_.size());}
  int indexOf(char ch,size_t from=0)const{return index(value_.find(ch,from));}
  int indexOf(const String& text,size_t from=0)const{return index(value_.find(text.value_,from));}
  int lastIndexOf(char ch)const{return index(value_.rfind(ch));}
  bool startsWith(const String& text)const{return value_.compare(0,text.length(),text.value_)==0;}
  bool endsWith(const String& text)const{return length()>=text.length()&&value_.compare(length()-text.length(),text.length(),text.value_)==0;}
  void trim(){
    auto begin=value_.find_first_not_of(" \t\n\r\v\f");
    if(begin==std::string::npos){value_.clear();return;}
    auto end=value_.find_last_not_of(" \t\n\r\v\f");value_=value_.substr(begin,end-begin+1);
  }
  void toLowerCase(){for(char& c:value_)c=char(std::tolower(static_cast<unsigned char>(c)));}
  void remove(size_t from,size_t n=std::string::npos){if(from<length())value_.erase(from,n);}
  String& operator+=(const String& other){value_+=other.value_;return *this;}
  String& operator+=(char other){value_+=other;return *this;}
  friend String operator+(const String& a,const String& b){return String(a.value_+b.value_);}
  friend bool operator==(const String& a,const String& b){return a.value_==b.value_;}
  friend bool operator!=(const String& a,const String& b){return !(a==b);}
  friend bool operator<(const String& a,const String& b){return a.value_<b.value_;}
  explicit operator bool()const{return !value_.empty();}
 private:
  static int index(size_t n){return n==std::string::npos?-1:static_cast<int>(n);}
  std::string value_;
};

inline uint32_t testClock=0,testRandom=0x12345678;
inline uint32_t millis(){return testClock;}
inline void delay(uint32_t ms){testClock+=ms;}
// Reproducible randomness, including the high bits, across AP restarts.
inline uint32_t esp_random(){
  testRandom^=testRandom<<13;testRandom^=testRandom>>17;testRandom^=testRandom<<5;
  return testRandom;
}
inline void esp_fill_random(void* data,size_t size){
  auto* out=static_cast<uint8_t*>(data);
  for(size_t i=0;i<size;++i)out[i]=static_cast<uint8_t>(esp_random());
}
