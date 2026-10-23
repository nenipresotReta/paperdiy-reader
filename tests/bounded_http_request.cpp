#include <cassert>
#include <iostream>
#include <string>
#include "bounded_http_request.h"

namespace {
BoundedHttpRequest parse(const std::string& text) {
  BoundedHttpRequest request;
  for (char c : text) request.feed(c);
  return request;
}
std::string post(const std::string& body, const std::string& extra = "") {
  return "POST /api/upload/chunk HTTP/1.1\r\nHost: 192.168.4.1\r\n"
         "Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}
void rejected(const std::string& text, int code) {
  auto request = parse(text);
  assert(request.state == BoundedHttpRequest::State::Failed);
  assert(request.error == code);
}
}

int main() {
  auto request = parse("GET /?t=0123456789 HTTP/1.1\r\nHost: 192.168.4.1\r\nCookie: diy_token=x\r\n\r\n");
  assert(request.state == BoundedHttpRequest::State::Ready && !request.post);
  assert(request.path == "/" && request.arguments.size() == 1);
  assert(request.arguments[0].first == "t" && request.arguments[0].second == "0123456789");
  request = parse(post("name=%E4%B8%AD%E6%96%87+book.epub&data=QQ%2B%2F%3D&offset=0"));
  assert(request.state == BoundedHttpRequest::State::Ready && request.post);
  assert(request.arguments[0].second == "中文 book.epub");
  assert(request.arguments[1].second == "QQ+/=");
  assert(request.arguments[2].second == "0");
  assert(parse("GET /api/status HTTP/1.0\r\n\r\n").state == BoundedHttpRequest::State::Ready);
  assert(parse(post("")).state == BoundedHttpRequest::State::Ready);

  // Every prefix of a valid request stays incomplete, never reaches a route.
  const auto complete = post("data=" + std::string(16384, 'A'));
  BoundedHttpRequest incremental;
  for (size_t i = 0; i < complete.size(); ++i) {
    const auto state = incremental.feed(complete[i]);
    assert(state == (i + 1 == complete.size() ? BoundedHttpRequest::State::Ready :
                                               BoundedHttpRequest::State::Reading));
  }
  assert(incremental.arguments[0].second.size() == 16384);

  rejected("POST /api/upload HTTP/1.1\r\nHost: x\r\nContent-Length: 50000000\r\n"
           "Content-Type: multipart/form-data; boundary=x\r\n\r\n", 413);
  rejected("POST /api/upload HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n"
           "Content-Type: multipart/form-data; boundary=x\r\n\r\n", 415);
  rejected(post("x=1", "Transfer-Encoding: chunked\r\n"), 400);
  rejected(post("x=1", "Content-Length: 3\r\n"), 400);
  rejected(post("x=1", "Content-Type: application/json\r\n"), 400);
  rejected(post("x=1", "Expect: 100-continue\r\n"), 417);
  rejected(post("x=1", "Cookie: a=b\r\nCookie: c=d\r\n"), 400);
  rejected("POST / HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n", 411);
  rejected("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx", 400);
  for (const char* length : {"-1", "+1", "2x", "", "1, 1"})
    rejected(std::string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ") + length + "\r\n\r\n", 400);
  rejected("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999999999999999999\r\n\r\n", 413);
  rejected(post("data=" + std::string(BoundedHttpRequest::MAX_BODY, 'A')), 413);
  auto maximum = post("data=" + std::string(BoundedHttpRequest::MAX_BODY - 5, 'A'));
  assert(parse(maximum).state == BoundedHttpRequest::State::Ready);
  rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Large: " + std::string(3072, 'x') + "\r\n\r\n", 431);

  for (const char* data : {"x=%00", "x=%GG", "x=%", "x=%0", "x=1&x=2", "x=1&", "=v", "novalue"})
    rejected(post(data), 400);
  rejected(post(std::string("x=a\0b", 5)), 400);
  std::string many;
  for (unsigned i = 0; i < 17; ++i) many += (i ? "&" : "") + std::string("k") + std::to_string(i) + "=v";
  rejected(post(many), 400);
  rejected(post(std::string(65, 'k') + "=v"), 400);
  for (const char* first : {"PATCH / HTTP/1.1", "HEAD / HTTP/1.1"})
    rejected(std::string(first) + "\r\nHost: x\r\n\r\n", 405);
  for (const char* first : {"GET http://x/ HTTP/1.1", "GET /#fragment HTTP/1.1", "GET / HTTP/2.0", "GET / HTTP/1.1 extra", "POST /?x=v HTTP/1.1"})
    rejected(std::string(first) + "\r\nHost: x\r\n\r\n", 400);
  rejected("GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", 400);
  rejected("GET / HTTP/1.1\r\n\r\n", 400);
  rejected("GET / HTTP/1.1\nHost: x\n\n", 400);
  rejected("GET / HTTP/1.1\rX", 400);
  rejected("GET / HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n", 400);

  // Bytes after the declared request never become a second parsed request.
  request = parse(post("x=v") + "POST /evil HTTP/1.1\r\n\r\n");
  assert(request.state == BoundedHttpRequest::State::Ready && request.arguments.size() == 1);
  std::cout << "bounded HTTP parser: framing, limits, URL decoding, partial delivery and rejection passed\n";
}
