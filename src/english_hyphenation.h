#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "en_us_patterns.h"

namespace english_hyphenation {
constexpr size_t MAX_WORD = 48;
inline bool letter(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
// Liang pattern matching. All tables stay in flash; scratch space is bounded.
// Return bit n when a hyphen may precede letter n. Minima follow the source:
// at least two letters before a hyphen and three letters after it.
inline uint64_t points(const char* word, size_t size) {
  if (size < 6 || size > MAX_WORD) return 0;
  char key[MAX_WORD + 3] = {'.'};
  bool lower = false;
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = word[i];
    if (!letter(c)) return 0;
    lower |= c >= 'a' && c <= 'z';
    key[i + 1] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
  }
  if (!lower) return 0; // Acronyms are not ordinary English words.
  key[size + 1] = '.';
  uint64_t result = 0;
  bool exception = false;
  for (const auto& item : english_patterns::EXCEPTIONS) {
    if (strlen(item.word) == size && !memcmp(item.word, key + 1, size)) {
      result = item.breaks; exception = true; break;
    }
  }
  if (!exception) {
    uint8_t scores[MAX_WORD + 4] = {};
    for (size_t start = 0; start < size + 2; ++start) {
      uint16_t node = 0;
      for (size_t at = start; at < size + 2; ++at) {
        const auto& parent = english_patterns::NODES[node];
        unsigned low = parent.child, high = low + parent.count;
        while (low < high) {
          unsigned mid = low + (high - low) / 2;
          if (english_patterns::NODES[mid].letter < key[at]) low = mid + 1;
          else high = mid;
        }
        if (low == unsigned(parent.child) + parent.count ||
            english_patterns::NODES[low].letter != key[at]) break;
        node = uint16_t(low);
        uint16_t offset = english_patterns::NODES[node].values;
        if (offset == UINT16_MAX) continue;
        uint8_t count = english_patterns::VALUES[offset++];
        for (unsigned i = 0; i < count && start + i < sizeof(scores); ++i) {
          uint8_t value = english_patterns::VALUES[offset + i];
          if (value > scores[start + i]) scores[start + i] = value;
        }
      }
    }
    for (size_t i = 2; i + 3 <= size; ++i)
      if (scores[i + 1] & 1) result |= uint64_t(1) << i;
  }
  uint64_t allowed = 0;
  for (size_t i = 2; i + 3 <= size; ++i) allowed |= uint64_t(1) << i;
  return result & allowed;
}
} // namespace english_hyphenation
