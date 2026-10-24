#pragma once
#include "epub_document.h"
#include "pagination.h"
#include "content_source.h"
namespace epub_table {
struct Geometry{int left=24,right=510,top=24,bottom=916,lineHeight=40,pageTop=24;};
struct Result{size_t next=0,fallbackEnd=0;bool fallback=false;int bottom=24;};
// Rectangular 2–4-column tables, row-atomic fragmentation. Source bytes are
// never synthesized and a whole unfit row returns to canonical prose.
template<class Renderer,class Tick>
int cell(const epub_document::Document& doc,const epub_objects::Cell& cell,bool header,int x,int top,int width,bool draw,const Geometry& geometry,Renderer& renderer,Tick tick){
  constexpr int PAD=8;const char* text=doc.text();size_t at=cell.begin;int y=top;
  pagination::Geometry lineGeometry;lineGeometry.left=0;lineGeometry.right=width-PAD*2;lineGeometry.hyphenate=false;lineGeometry.justify=false;lineGeometry.hangLimit=1;lineGeometry.strictBreaks=true;
  auto styleAt=[&](size_t p){auto run=doc.run(doc.runAt(p));if(header)run.flags|=markdown::Strong;return run;};
  auto measure=[&](const char* ch,size_t bytes){const uintptr_t address=reinterpret_cast<uintptr_t>(ch),base=reinterpret_cast<uintptr_t>(text);size_t p=address>=base+cell.begin&&address<base+cell.end?size_t(address-base):cell.begin;auto run=styleAt(p);return renderer.glyph(run,ch,bytes).width;};
  if(at==cell.end)return geometry.lineHeight;
  while(at<cell.end){const auto line=pagination::line(text,cell.end,at,lineGeometry,measure,tick,false);if(line.next<=at||line.advance>lineGeometry.right+1)return -1;
    int ascent=0,descent=0;for(size_t p=line.begin;p<line.end;){const size_t bytes=pagination::utf8Bytes(text+p,line.end-p);auto run=styleAt(p);const auto box=renderer.glyph(run,text+p,bytes);ascent=std::max(ascent,box.ascent);descent=std::max(descent,box.descent);p+=bytes;}
    const int height=std::max(geometry.lineHeight,ascent+descent+1);if(y+height-top>geometry.bottom-geometry.pageTop-PAD*2)return -1;
    if(draw){int pen=x+PAD;for(size_t p=line.begin;p<line.end;){const size_t bytes=pagination::utf8Bytes(text+p,line.end-p);auto run=styleAt(p);renderer.drawGlyph(run,text+p,bytes,pen,y+ascent);pen+=pagination::advance(text+p,bytes,pagination::codepoint(text+p,bytes),measure);p+=bytes;}}
    y+=height;at=line.next;tick();
  }return y-top;
}
template<class Renderer,class Tick>
Result page(const epub_document::Document& doc,size_t from,bool draw,const Geometry& geometry,Renderer& renderer,Tick tick){
  Result result;result.next=from;result.bottom=geometry.top;const size_t index=doc.tableAt(from);if(index==doc.tableCount())return result;const auto& table=doc.table(index);
  const int width=(geometry.right-geometry.left)/table.columns;size_t rowIndex=table.firstRow;
  while(rowIndex<table.firstRow+table.rows&&doc.row(rowIndex).end<=from)++rowIndex;
  int y=geometry.top;constexpr int PAD=8;
  for(;rowIndex<table.firstRow+table.rows;++rowIndex){const auto& row=doc.row(rowIndex);const size_t next=rowIndex+1<table.firstRow+table.rows?doc.row(rowIndex+1).begin:table.end;
    if(row.begin<reader_content::begin()||row.end>=readerContentLimit()&&readerContentLimit()<doc.size())return result;
    int height=0;bool fits=width>PAD*2+28;
    for(size_t j=0;j<row.cells&&fits;++j){int measured=cell(doc,doc.cell(row.firstCell+j),row.flags&1,0,0,width,false,geometry,renderer,tick);if(measured<0)fits=false;else height=std::max(height,measured);}
    height+=PAD*2;
    if(!fits||height>geometry.bottom-geometry.pageTop){if(y>geometry.pageTop)return result;result.fallback=true;result.fallbackEnd=next;return result;}
    if(y+height>geometry.bottom)return result;
    if(draw){for(size_t j=0;j<row.cells;++j){cell(doc,doc.cell(row.firstCell+j),row.flags&1,geometry.left+int(j)*width,y+PAD,width,true,geometry,renderer,tick);renderer.rule(geometry.left+int(j)*width,y,1,height);}
      renderer.rule(geometry.left,y,int(table.columns)*width,1);renderer.rule(geometry.left,y+height-1,int(table.columns)*width,1);renderer.rule(geometry.left+int(table.columns)*width-1,y,1,height);
    }
    result.next=next;y+=height;result.bottom=y;tick();
  }return result;
}
} // namespace epub_table
