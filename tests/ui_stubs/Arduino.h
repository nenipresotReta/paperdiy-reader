#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Minimal Arduino String for the host UI simulation. It implements only the
// operations used by the reader/UI sources and intentionally stays a value type.
class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}
  String(int value) : value_(std::to_string(value)) {}
  String(unsigned value) : value_(std::to_string(value)) {}
  String(size_t value) : value_(std::to_string(value)) {}

  String& operator=(const char* value) {
    value_ = value ? value : "";
    return *this;
  }
  String& operator+=(const char* value) {
    value_ += value ? value : "";
    return *this;
  }
  String& operator+=(const String& value) {
    value_ += value.value_;
    return *this;
  }
  String operator+(const char* value) const {
    String out(*this);
    out += value;
    return out;
  }
  String operator+(const String& value) const {
    String out(*this);
    out += value;
    return out;
  }
  bool operator==(const char* value) const { return value_ == (value ? value : ""); }
  bool operator!=(const char* value) const { return !(*this == value); }
  bool operator==(const String& value) const { return value_ == value.value_; }
  bool operator!=(const String& value) const { return value_ != value.value_; }

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  String substring(size_t from) const {
    if (from > value_.size()) from = value_.size();
    return String(value_.substr(from));
  }
  String substring(size_t from, size_t to) const {
    if (from > value_.size()) from = value_.size();
    if (to > value_.size()) to = value_.size();
    if (to < from) to = from;
    return String(value_.substr(from, to - from));
  }
  int lastIndexOf(char ch) const {
    const size_t at = value_.rfind(ch);
    return at == std::string::npos ? -1 : int(at);
  }
  operator const char*() const { return value_.c_str(); }

 private:
  std::string value_;
};

inline uint32_t testClock = 0;
inline uint32_t millis() { return testClock; }
inline void delay(uint32_t elapsed) { testClock += elapsed; }

struct TestSerial {
  void println(const char*) {}
  template <class... Args>
  void printf(const char*, Args...) {}
};
inline TestSerial Serial;
