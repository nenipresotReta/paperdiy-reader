#pragma once
#include <cstddef>
#include <cstdint>

namespace lgfx {
struct IFont {};
}

// Match the pinned M5GFX constants: signed RGB565, not unsigned RGB888.
constexpr int TFT_BLACK = 0x0000;
constexpr int TFT_WHITE = 0xFFFF;

enum class epd_mode_t { epd_quality, epd_fast };
