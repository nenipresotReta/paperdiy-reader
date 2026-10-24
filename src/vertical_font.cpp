#include "vertical_font.h"
#include "vertical_font_data.h"
namespace vertical_font {
const reader_font::Font& font(unsigned pixels){switch(pixels){case 12:return data::FONT_12;case 14:return data::FONT_14;case 24:return data::FONT_24;default:return data::FONT_28;}}
bool sideways(uint32_t cp,unsigned pixels){
  struct Range{uint32_t first,last;bool transformed;};static const Range ranges[]={
#include "vertical_orientation_data.h"
  };
  size_t first=0,last=sizeof(ranges)/sizeof(ranges[0]);while(first<last){size_t m=(first+last)/2;if(ranges[m].last<cp)first=m+1;else last=m;}
  // Tr characters use a baked vertical alternate when it exists.
  return first<sizeof(ranges)/sizeof(ranges[0])&&ranges[first].first<=cp&&(!ranges[first].transformed||!has(cp,pixels));
}
}
