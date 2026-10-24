// Host content preparation using the exact firmware EPUB importer. No private
// source text is logged, no source is modified, and no target file is replaced.
#include "epub_core.h"
#include "epub_document.h"
#include "prepared_epub.h"
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

bool digestFile(FILE* file, uint64_t expected, uint8_t (&digest)[32]) {
  struct stat before{}, after{};
  if (fstat(fileno(file), &before) || !S_ISREG(before.st_mode) ||
      before.st_size < 0 || uint64_t(before.st_size) != expected || std::fseek(file, 0, SEEK_SET)) return false;
  Digest hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) return false;
  std::array<uint8_t, 65536> chunk{};
  for (uint64_t at = 0; at < expected;) {
    size_t count = std::min<uint64_t>(chunk.size(), expected - at);
    if (std::fread(chunk.data(), 1, count, file) != count ||
        EVP_DigestUpdate(hash.get(), chunk.data(), count) != 1) return false;
    at += count;
  }
  unsigned bytes = 0;
  return EVP_DigestFinal_ex(hash.get(), digest, &bytes) == 1 && bytes == 32 &&
      !fstat(fileno(file), &after) && after.st_size == before.st_size && after.st_mtime == before.st_mtime;
}
bool digestPayload(const char* text, size_t bytes, const std::vector<uint8_t>& metadata,
                   uint8_t (&digest)[32]) {
  Digest hash(EVP_MD_CTX_new(), EVP_MD_CTX_free); unsigned count = 0;
  return hash && EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(hash.get(), text, bytes) == 1 &&
      (metadata.empty() || EVP_DigestUpdate(hash.get(), metadata.data(), metadata.size()) == 1) &&
      EVP_DigestFinal_ex(hash.get(), digest, &count) == 1 && count == 32;
}
bool writeAll(int fd, const void* data, size_t bytes) {
  const uint8_t* cursor = static_cast<const uint8_t*>(data);
  while (bytes) {
    ssize_t count = ::write(fd, cursor, bytes);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    cursor += count; bytes -= size_t(count);
  }
  return true;
}
bool publish(const std::filesystem::path& destination, const uint8_t* header,
             const char* text, size_t bytes, const std::vector<uint8_t>& metadata) {
  // A hard-link publication within the same directory is atomic and refuses
  // to overwrite an existing name, including a concurrent writer's file.
  auto folder = destination.parent_path(); if (folder.empty()) folder = ".";
  std::string pattern = (folder / ".paperdiy-prepare-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
  int fd = mkstemp(name.data()); if (fd < 0) return false;
  bool ok = writeAll(fd, header, prepared_epub::HEADER_BYTES) && writeAll(fd, text, bytes) &&
      writeAll(fd, metadata.data(), metadata.size()) && fsync(fd) == 0;
  if (close(fd)) ok = false;
  if (ok) ok = link(name.data(), destination.c_str()) == 0;
  unlink(name.data()); return ok;
}
std::string hex(const uint8_t* bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t i = 0; i < 32; ++i) { result[i * 2] = digits[bytes[i] >> 4]; result[i * 2 + 1] = digits[bytes[i] & 15]; }
  return result;
}
int fail(const char* message) { std::fprintf(stderr, "prepare_epub: %s\n", message); return 1; }
} // namespace

int prepare(int argc, char** argv) {
  bool flat = false;
  if (argc == 4 && !std::strcmp(argv[3], "--flat")) flat = true;
  else if (argc != 3) {
    std::fprintf(stderr, "Usage: prepare_epub INPUT.epub OUTPUT.pdt [--flat]\n"); return 2;
  }
  std::filesystem::path source(argv[1]), destination(argv[2]);
  if (std::filesystem::exists(destination)) return fail("output exists; choose a new destination");
  File input(std::fopen(source.c_str(), "rb"), std::fclose);
  if (!input) return fail("cannot open source");
  struct stat info{};
  if (fstat(fileno(input.get()), &info) || !S_ISREG(info.st_mode) || info.st_size < 22 ||
      uint64_t(info.st_size) > prepared_epub::MAX_SOURCE_BYTES) return fail("source must be a regular EPUB up to 64 MiB");
  prepared_epub::Header header; header.sourceBytes = uint64_t(info.st_size);
  if (!digestFile(input.get(), header.sourceBytes, header.sourceDigest)) return fail("cannot fingerprint source");
  std::unique_ptr<char, decltype(&std::free)> text(static_cast<char*>(std::malloc(prepared_epub::MAX_TEXT_BYTES + 1)), std::free);
  if (!text) return fail("cannot allocate text buffer");
  epub::Importer importer;
  epub::structure::Collector structure;
  importer.fileSize = uint32_t(header.sourceBytes); importer.output = text.get();
  importer.capacity = prepared_epub::MAX_TEXT_BYTES; importer.alloc = std::malloc;
  importer.structure = flat ? nullptr : &structure;
  importer.read = [&](uint32_t offset, void* out, size_t bytes) -> size_t {
    if (std::fseek(input.get(), long(offset), SEEK_SET)) return 0;
    return std::fread(out, 1, bytes, input.get());
  };
  if (!importer.run()) return fail(importer.error.c_str());
  header.textBytes = uint32_t(importer.used);
  header.flags = uint32_t(importer.language);
  std::vector<uint8_t> metadata;
  epub_document::Document semantic;
  if (structure.ready() && semantic.assign(structure, text.get(), importer.used)) {
    try { if (!semantic.encode(metadata)) metadata.clear(); }
    catch (const std::bad_alloc&) { metadata.clear(); }
  }
  if (metadata.size() > prepared_epub::MAX_METADATA_BYTES) metadata.clear();
  // A local foreign-language span cannot safely inherit package-wide English
  // patterns if the optional semantic representation was lost to allocation.
  if (metadata.empty() && importer.unsafeFlatLanguage)
    header.flags = uint32_t(text_language::Language::None);
  header.metadataBytes = uint32_t(metadata.size());
  epub_document::Document checked;
  if (!metadata.empty() && !checked.decode(metadata.data(), metadata.size(), text.get(), importer.used))
    return fail("generated metadata failed validation");
  uint8_t after[32];
  if (!digestFile(input.get(), header.sourceBytes, after) || std::memcmp(after, header.sourceDigest, 32))
    return fail("source changed during preparation");
  if (!digestPayload(text.get(), importer.used, metadata, header.payloadDigest)) return fail("cannot hash prepared payload");
  std::array<uint8_t, prepared_epub::HEADER_BYTES> bytes{};
  if (!prepared_epub::encode(header, bytes.data(), bytes.size())) return fail("invalid prepared envelope");
  if (!publish(destination, bytes.data(), text.get(), importer.used, metadata)) return fail("cannot publish output without replacement");
  std::printf("{\"format\":\"PDT1\",\"extractor\":%u,\"sourceBytes\":%llu,\"textBytes\":%u,\"metadataBytes\":%u,\"flags\":%u,\"sourceSha256\":\"%s\",\"payloadSha256\":\"%s\"}\n",
      prepared_epub::EXTRACTOR_ID, static_cast<unsigned long long>(header.sourceBytes), header.textBytes,
      header.metadataBytes, header.flags, hex(header.sourceDigest).c_str(), hex(header.payloadDigest).c_str());
  return 0;
}

int main(int argc, char** argv) {
  try { return prepare(argc, argv); }
  catch (const std::exception&) { return fail("preparation failed; source is unchanged"); }
}
