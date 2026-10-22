#pragma once
#include <cstddef>
#include <cstdint>

namespace lgfx {
struct IFont {};
}

constexpr uint32_t TFT_BLACK = 0x000000u;
constexpr uint32_t TFT_WHITE = 0xFFFFFFu;

enum class epd_mode_t { epd_quality, epd_fast };
