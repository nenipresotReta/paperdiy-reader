#pragma once
#include <cstddef>
#include <cstdint>

namespace bookmarks {
enum class SaveResult { Saved, Unavailable, PositionFailed, ResumeFailed };

// Keep persistence separate from drawing. Failed writes must remain retryable.
template<class Preferences>
SaveResult save(Preferences& prefs, bool available, const char* key,
                uint32_t offset, const char* path, size_t pathLength) {
  if (!available) return SaveResult::Unavailable;
  if (prefs.getUInt(key, UINT32_MAX) != offset &&
      prefs.putUInt(key, offset) != sizeof(uint32_t))
    return SaveResult::PositionFailed;
  if (prefs.getString("sdLast", "") != path &&
      prefs.putString("sdLast", path) != pathLength)
    return SaveResult::ResumeFailed;
  return SaveResult::Saved;
}

// Version and length framing prevent path/text boundary collisions. The legacy
// digest is still computed at import time so existing NVS keys remain usable.
template<class Update>
void identity(Update update, const char* path, size_t pathLength,
              const char* text, size_t length, bool legacy = false) {
  if (!legacy) {
    static constexpr uint8_t domain[] = {'P','D','R',2};
    uint8_t size[4];
    for (unsigned i = 0; i < 4; ++i) size[i] = uint32_t(pathLength) >> (i * 8);
    update(domain, sizeof(domain));
    update(size, sizeof(size));
  }
  update(reinterpret_cast<const uint8_t*>(path), pathLength);
  for (size_t p = 0; p < length; p += 4096)
    update(reinterpret_cast<const uint8_t*>(text + p),
           length - p < 4096 ? length - p : 4096);
}
}
