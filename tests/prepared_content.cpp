#include "prepared_epub.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
  using namespace prepared_epub;
  Header h; h.sourceBytes = 12345; h.textBytes = 123; h.metadataBytes = 48;
  h.flags = BRITISH;
  for (unsigned i = 0; i < 32; ++i) { h.sourceDigest[i] = uint8_t(i); h.payloadDigest[i] = uint8_t(255-i); }
  std::array<uint8_t, HEADER_BYTES> bytes{};
  assert(encode(h, bytes.data(), bytes.size()));
  Header result; assert(decode(bytes.data(), bytes.size(), fileBytes(h), result));
  assert(result.sourceBytes == h.sourceBytes && result.textBytes == h.textBytes &&
    result.metadataBytes == h.metadataBytes && result.flags == BRITISH);
  assert(!memcmp(result.sourceDigest, h.sourceDigest, 32) && !memcmp(result.payloadDigest, h.payloadDigest, 32));
  auto reject = [&](size_t at, uint8_t value) {
    auto bad = bytes; bad[at] = value;
    Header unchanged; unchanged.sourceBytes = 777;
    assert(!decode(bad.data(), bad.size(), fileBytes(h), unchanged));
    assert(unchanged.sourceBytes == 777);
  };
  reject(0, 'X'); reject(4, 2); reject(8, 159); reject(12, 255);
  reject(20, 1); reject(96, 3); reject(96, 255);
  for (size_t at = 100; at < HEADER_BYTES; ++at) reject(at, 1);
  assert(!decode(nullptr, HEADER_BYTES, fileBytes(h), result));
  assert(!decode(bytes.data(), HEADER_BYTES - 1, fileBytes(h), result));
  assert(!decode(bytes.data(), HEADER_BYTES, fileBytes(h) - 1, result));
  assert(!decode(bytes.data(), HEADER_BYTES, fileBytes(h) + 1, result));
  for (auto kind : {0u, 1u, 2u}) { h.flags = kind; assert(encode(h, bytes.data(), bytes.size())); }
  h.flags = 3; assert(!encode(h, bytes.data(), bytes.size())); h.flags = 0;
  h.sourceBytes = 21; assert(!valid(h)); h.sourceBytes = MAX_SOURCE_BYTES; assert(valid(h));
  ++h.sourceBytes; assert(!valid(h)); h.sourceBytes = 100;
  h.textBytes = 0; assert(!valid(h)); h.textBytes = MAX_TEXT_BYTES; assert(valid(h));
  ++h.textBytes; assert(!valid(h)); h.textBytes = 100;
  h.metadataBytes = MAX_METADATA_BYTES; assert(valid(h)); ++h.metadataBytes; assert(!valid(h));
  std::puts("prepared content envelope checks passed");
}
