#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "settings_registry.h"

// A shutdown snapshot is one atomic NVS value. Legacy keys remain read-only
// fallbacks until the first successful save; selected books are identified by path.
namespace resume_state {
constexpr size_t MAX_PATH = 1024, HEADER = 8, MAX_BYTES = HEADER + MAX_PATH;
constexpr const char* KEY = "resumeV2";
constexpr const char* LEGACY_KEY = "resumeV1";
enum class ReadResult { Absent, Valid, Invalid };
struct Record {
  uint8_t screen = 0;
  SettingsId settingsId = SettingsId::None;
  char path[MAX_PATH + 1] = {};
};
inline bool encode(const Record& value, uint8_t* bytes, size_t& size) {
  const void* end = std::memchr(value.path, 0, sizeof(value.path));
  if (!end || value.screen > 2 || !settings_registry::valid(value.settingsId)) return false;
  const size_t length = static_cast<const char*>(end) - value.path;
  if (length && value.path[0] != '/') return false;
  bytes[0]='P'; bytes[1]='R'; bytes[2]='S'; bytes[3]=2;
  bytes[4]=value.screen; bytes[5]=uint8_t(value.settingsId);
  bytes[6]=uint8_t(length); bytes[7]=uint8_t(length >> 8);
  std::memcpy(bytes+HEADER,value.path,length); size=HEADER+length;
  return true;
}
inline bool decode(const uint8_t* bytes, size_t size, Record& value) {
  if (size<HEADER || size>MAX_BYTES || std::memcmp(bytes,"PRS",3) ||
      (bytes[3]!=1 && bytes[3]!=2) || bytes[4]>2) return false;
  if (bytes[3]==1 && bytes[5]>1) return false;
  // Older V2 snapshots may select the removed typography page. Restore the
  // remaining settings book without rewriting NVS or disturbing its book path.
  const auto id=bytes[3]==2 && bytes[5]==settings_registry::RETIRED_TYPOGRAPHY_ID
    ? SettingsId::Network : static_cast<SettingsId>(bytes[5]);
  if (!settings_registry::valid(id)) return false;
  const size_t length=size_t(bytes[6])|(size_t(bytes[7])<<8);
  if (length!=size-HEADER || (length && bytes[HEADER]!='/') ||
      std::memchr(bytes+HEADER,0,length)) return false;
  Record next; next.screen=bytes[4]; next.settingsId=id;
  std::memcpy(next.path,bytes+HEADER,length); value=next;
  return true;
}
template<class Preferences>
ReadResult read(Preferences& prefs, Record& value) {
  // A corrupt newer record must not resurrect a stale V1 shutdown snapshot.
  const bool newer=prefs.isKey(KEY);
  const char* key=newer?KEY:LEGACY_KEY;
  if (!prefs.isKey(key)) return ReadResult::Absent;
  uint8_t bytes[MAX_BYTES]; const size_t size=prefs.getBytesLength(key);
  if (size<HEADER || size>sizeof(bytes) || prefs.getBytes(key,bytes,size)!=size ||
      bytes[3]!=(newer?2:1) || !decode(bytes,size,value)) return ReadResult::Invalid;
  return ReadResult::Valid;
}
template<class Preferences>
bool save(Preferences& prefs, const Record& value) {
  uint8_t bytes[MAX_BYTES], check[MAX_BYTES]; size_t size=0;
  if (!encode(value,bytes,size)) return false;
  if (prefs.getBytesLength(KEY)==size && prefs.getBytes(KEY,check,size)==size &&
      !std::memcmp(bytes,check,size)) return true;
  if (prefs.putBytes(KEY,bytes,size)!=size) return false;
  return prefs.getBytesLength(KEY)==size && prefs.getBytes(KEY,check,size)==size &&
      !std::memcmp(bytes,check,size);
}
template<class Books>
size_t selectedIndex(const Record& value, const Books& books) {
  for (size_t i=0;i<books.size();++i)
    if (!std::strcmp(books[i].c_str(),value.path)) return i;
  return 0;
}
template<class Preferences>
bool saveRotation(Preferences& prefs, bool rotated) {
  const uint32_t desired=rotated?1:0;
  if (prefs.getUInt("rot180",UINT32_MAX)==desired) return true;
  return prefs.putUInt("rot180",desired)==sizeof(uint32_t) &&
      prefs.getUInt("rot180",UINT32_MAX)==desired;
}
}
