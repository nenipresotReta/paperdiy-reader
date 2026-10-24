#pragma once
#include <cstddef>

// Shared wire admission limits. Prepared content and the semantic decoder
// must admit exactly the same bounded PDS payloads.
namespace epub_metadata_limits {
constexpr size_t MAX_RUNS=16384;
constexpr size_t MAX_RUBY=2048;
constexpr size_t V1_HEADER_BYTES=16, V2_HEADER_BYTES=24;
constexpr size_t RUN_BYTES=16, RUBY_BYTES=20;
constexpr size_t V3_HEADER_BYTES=40,MATH_BYTES=16,TABLE_BYTES=16,ROW_BYTES=16,CELL_BYTES=8;
constexpr size_t MAX_MATH=256,MAX_TABLES=32,MAX_ROWS=256,MAX_CELLS=1024,MAX_TEX_BYTES=32768;
constexpr size_t MAX_BYTES=V3_HEADER_BYTES+MAX_RUNS*RUN_BYTES+MAX_RUBY*RUBY_BYTES+MAX_MATH*MATH_BYTES+MAX_TABLES*TABLE_BYTES+MAX_ROWS*ROW_BYTES+MAX_CELLS*CELL_BYTES+MAX_TEX_BYTES;
}
