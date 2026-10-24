#pragma once
// Portable interchange for optional, locally prepared EPUB content. This is
// not a page cache and carries no device path, reading position or credentials.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "epub_metadata_limits.h"

namespace prepared_epub {
constexpr uint32_t VERSION = 1;
// v2 records source-declared vertical/ruby/MathML/table semantics. v1 text
// anchors remain stable, but its missing annotations require a source reimport.
constexpr uint32_t EXTRACTOR_ID = 2;
constexpr size_t HEADER_BYTES = 160;
constexpr uint64_t MAX_SOURCE_BYTES = 64ull * 1024 * 1024;
constexpr uint32_t MAX_TEXT_BYTES = 4u * 1024 * 1024;
constexpr uint32_t MAX_METADATA_BYTES = uint32_t(epub_metadata_limits::MAX_BYTES);
// Wire language enum: None=0, EnglishUS=1, EnglishGB=2; no bit combinations.
constexpr uint32_t ENGLISH = 1u, BRITISH = 2u;

struct Header {
  uint64_t sourceBytes = 0;
  uint32_t textBytes = 0, metadataBytes = 0, flags = 0;
  uint8_t sourceDigest[32] = {}, payloadDigest[32] = {};
};
inline uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void write32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}
inline bool valid(const Header& h) {
  return h.sourceBytes >= 22 && h.sourceBytes <= MAX_SOURCE_BYTES &&
      h.textBytes && h.textBytes <= MAX_TEXT_BYTES &&
      h.metadataBytes <= MAX_METADATA_BYTES && h.flags <= BRITISH;
}
inline uint64_t fileBytes(const Header& h) {
  return HEADER_BYTES + uint64_t(h.textBytes) + h.metadataBytes;
}
inline bool encode(const Header& h, uint8_t* out, size_t size) {
  if (!out || size != HEADER_BYTES || !valid(h)) return false;
  std::memset(out, 0, size); std::memcpy(out, "PDT1", 4);
  write32(out + 4, VERSION); write32(out + 8, HEADER_BYTES);
  write32(out + 12, EXTRACTOR_ID);
  write32(out + 16, uint32_t(h.sourceBytes));
  write32(out + 20, uint32_t(h.sourceBytes >> 32));
  write32(out + 24, h.textBytes); write32(out + 28, h.metadataBytes);
  std::memcpy(out + 32, h.sourceDigest, 32); std::memcpy(out + 64, h.payloadDigest, 32);
  write32(out + 96, h.flags); return true;
}
// Publication is all-or-nothing: a failure leaves the caller's Header intact.
// Payload hashing, UTF-8 and semantic validation remain the caller's job.
inline bool decode(const uint8_t* data, size_t size, uint64_t total, Header& out) {
  if (!data || size != HEADER_BYTES || std::memcmp(data, "PDT1", 4) ||
      read32(data + 4) != VERSION || read32(data + 8) != HEADER_BYTES ||
      read32(data + 12) != EXTRACTOR_ID) return false;
  for (size_t i = 100; i < HEADER_BYTES; ++i) if (data[i]) return false;
  Header next;
  next.sourceBytes = uint64_t(read32(data + 16)) | uint64_t(read32(data + 20)) << 32;
  next.textBytes = read32(data + 24); next.metadataBytes = read32(data + 28);
  next.flags = read32(data + 96);
  std::memcpy(next.sourceDigest, data + 32, 32); std::memcpy(next.payloadDigest, data + 64, 32);
  if (!valid(next) || total != fileBytes(next)) return false;
  out = next; return true;
}
} // namespace prepared_epub
