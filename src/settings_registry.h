#pragma once
#include <cstddef>
#include <cstdint>

// Persist these IDs, never their current position in the library registry.
enum class SettingsId : uint8_t { None=0, Network=1 };
namespace settings_registry {
// Wire ID 2 belonged to the removed typography settings page; never reuse it.
constexpr uint8_t RETIRED_TYPOGRAPHY_ID=2;
struct Entry { SettingsId id; const char* title; };
constexpr Entry ENTRIES[] = {
  {SettingsId::Network,"Network & Rotation"},
};
constexpr size_t COUNT=sizeof(ENTRIES)/sizeof(ENTRIES[0]);
inline bool valid(SettingsId id) {
  if(id==SettingsId::None)return true;
  for(const auto& entry:ENTRIES)if(entry.id==id)return true;
  return false;
}
inline size_t index(SettingsId id) {
  for(size_t i=0;i<COUNT;++i)if(ENTRIES[i].id==id)return i;
  return 0;
}
inline size_t libraryCursor(size_t bookIndex,bool settings,SettingsId id) {
  return settings?index(id):COUNT+bookIndex;
}
}
