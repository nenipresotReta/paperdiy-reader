#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The AP accepts small URL-encoded requests, not streaming multipart bodies.
// Keeping parsing independent of sockets makes its memory and framing limits
// explicit and lets the main loop service buttons between incoming chunks.
class BoundedHttpRequest {
 public:
  static constexpr size_t MAX_HEADERS = 3072;
  static constexpr size_t MAX_BODY = 17000;
  static constexpr size_t MAX_FIELDS = 16;
  using Field = std::pair<std::string, std::string>;
  enum class State { Reading, Ready, Failed };

  State state = State::Reading;
  int error = 0;
  bool post = false, http11 = true;
  std::string path;
  std::vector<Field> headers, arguments;
  size_t contentLength = 0;

  State feed(char byte) {
    if (state != State::Reading) return state;
    if (byte == '\0') return fail(400);
    if (!headersDone_) {
      if (head_.size() >= MAX_HEADERS) return fail(431);
      // Only CRLF line endings are accepted; stray controls cannot become
      // header boundaries or introduce ambiguous request framing.
      if ((byte == '\n' && (head_.empty() || head_.back() != '\r')) ||
          (!head_.empty() && head_.back() == '\r' && byte != '\n'))
        return fail(400);
      head_ += byte;
      const size_t n = head_.size();
      if (n >= 4 && head_.compare(n - 4, 4, "\r\n\r\n") == 0) {
        if (!parseHeaders()) return state;
        headersDone_ = true;
        if (!contentLength) finish();
        else body_.reserve(contentLength);
      }
    } else {
      body_ += byte;
      if (body_.size() == contentLength) finish();
    }
    return state;
  }

 private:
  std::string head_, body_, query_;
  bool headersDone_ = false;

  State fail(int code) { error = code; state = State::Failed; return state; }
  bool bad(int code) { fail(code); return false; }
  static bool token(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) return true;
    return std::strchr("!#$%&'*+-.^_`|~", char(c)) != nullptr;
  }
  static std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
  }
  static std::string trim(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t") - start + 1);
  }
  bool parseHeaders() {
    const size_t lineEnd = head_.find("\r\n");
    const std::string first = head_.substr(0, lineEnd);
    const size_t a = first.find(' '), b = first.find(' ', a == std::string::npos ? 0 : a + 1);
    if (a == std::string::npos || b == std::string::npos || first.find(' ', b + 1) != std::string::npos)
      return bad(400);
    const std::string method = first.substr(0, a), version = first.substr(b + 1);
    if (method != "GET" && method != "POST") return bad(405);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return bad(400);
    post = method == "POST";
    http11 = version == "HTTP/1.1";
    const std::string target = first.substr(a + 1, b - a - 1);
    if (target.empty() || target[0] != '/' || target.size() > 512) return bad(400);
    for (unsigned char c : target) if (c <= 32 || c >= 127 || c == '#') return bad(400);
    const size_t question = target.find('?');
    path = target.substr(0, question);
    if (question != std::string::npos) {
      if (post) return bad(400);  // POST arguments have exactly one source.
      query_ = target.substr(question + 1);
    }
    bool hasLength = false, hasType = false, hasHost = false;
    std::string contentType;
    size_t start = lineEnd + 2;
    while (start < head_.size() - 2) {
      const size_t end = head_.find("\r\n", start);
      if (end == std::string::npos || end == start || headers.size() >= 32) return bad(400);
      const std::string line = head_.substr(start, end - start);
      const size_t colon = line.find(':');
      if (colon == std::string::npos || !colon) return bad(400);
      for (size_t i = 0; i < colon; ++i) if (!token(uint8_t(line[i]))) return bad(400);
      const std::string name = lower(line.substr(0, colon));
      const std::string value = trim(line.substr(colon + 1));
      for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return bad(400);
      if (name == "transfer-encoding") return bad(400);
      if (name == "expect") return bad(417);
      if (name == "content-length") {
        if (hasLength || value.empty()) return bad(400);
        hasLength = true;
        for (char c : value) {
          if (c < '0' || c > '9') return bad(400);
          const unsigned digit = unsigned(c - '0');
          if (contentLength > (MAX_BODY - digit) / 10) return bad(413);
          contentLength = contentLength * 10 + digit;
        }
      } else if (name == "content-type") {
        if (hasType) return bad(400);
        hasType = true;
        contentType = lower(trim(value.substr(0, value.find(';'))));
      } else if (name == "host") {
        if (hasHost || value.empty()) return bad(400);
        hasHost = true;
      } else if (name == "cookie" || name == "authorization") {
        for (const auto& prior : headers) if (prior.first == name) return bad(400);
      }
      headers.emplace_back(name, value);
      start = end + 2;
    }
    if (http11 && !hasHost) return bad(400);
    if (post && !hasLength) return bad(411);
    if (post && (!hasType || contentType != "application/x-www-form-urlencoded")) return bad(415);
    if (!post && contentLength) return bad(400);
    return true;
  }
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static bool decode(const std::string& input, std::string& out) {
    for (size_t i = 0; i < input.size(); ++i) {
      char c = input[i];
      if (c == '%') {
        if (i + 2 >= input.size()) return false;
        const int hi = hex(input[i + 1]), lo = hex(input[i + 2]);
        if (hi < 0 || lo < 0 || !(hi || lo)) return false;
        c = char((hi << 4) | lo);
        i += 2;
      } else if (c == '+') c = ' ';
      out += c;
    }
    return true;
  }
  void finish() {
    const std::string& fields = post ? body_ : query_;
    size_t start = 0;
    while (start < fields.size()) {
      const size_t end = fields.find('&', start);
      const std::string item = fields.substr(start, end == std::string::npos ? end : end - start);
      const size_t equal = item.find('=');
      if (equal == std::string::npos || equal == 0 || arguments.size() >= MAX_FIELDS) { fail(400); return; }
      Field field;
      if (!decode(item.substr(0, equal), field.first) || field.first.size() > 64 ||
          !decode(item.substr(equal + 1), field.second)) { fail(400); return; }
      for (const auto& previous : arguments)
        if (previous.first == field.first) { fail(400); return; }
      arguments.push_back(std::move(field));
      if (end == std::string::npos) break;
      start = end + 1;
      if (start == fields.size()) { fail(400); return; }
    }
    state = State::Ready;
  }
};
