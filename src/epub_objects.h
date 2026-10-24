#pragma once
#include <cstdint>
namespace epub_objects {
struct Math {uint32_t begin,end,texOffset;uint16_t texBytes,flags;};
struct Cell {uint32_t begin,end;};
struct Row {uint32_t begin,end,firstCell;uint16_t cells,flags;};
struct Table {uint32_t begin,end,firstRow;uint16_t rows,columns;};
constexpr uint16_t MATH_DISPLAY=1;
constexpr unsigned MAX_MATH=256,MAX_TEX_BYTES=32768,MAX_TABLES=32,MAX_ROWS=256,MAX_CELLS=1024;
constexpr unsigned MAX_MATH_SOURCE=4096,MAX_ROW_SOURCE=8192;
}
