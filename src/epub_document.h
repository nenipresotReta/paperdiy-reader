#pragma once
// A bounded semantic view over canonical EPUB text. Never inserts presentation
// bytes: every navigation/bookmark offset still refers to the importer's text.
#include "epub_structure.h"
#include "epub_metadata_limits.h"
#include "epub_objects.h"
#include "markdown_document.h"
#include "text_language.h"
#include <algorithm>
#include <memory>
#include <new>
#include <vector>

namespace epub_document {
struct Ruby { uint32_t begin,baseEnd,noteBegin,noteEnd,end; };
class Document {
 public:
  enum class Error : uint8_t { None, Unavailable, Budget, Invalid, Allocation };
  static constexpr size_t MAX_RUNS=16384, HEADER_BYTES=16, RECORD_BYTES=16;
  static constexpr size_t MAX_RUBIES=2048, V2_HEADER_BYTES=24, RUBY_BYTES=20;
  static constexpr size_t MAX_BYTES=epub_metadata_limits::MAX_BYTES;
  static_assert(MAX_BYTES==epub_metadata_limits::MAX_BYTES,"EPUB semantic wire bound mismatch");
  void clear(){runs_.reset();modes_.reset();transitions_.reset();rubies_.reset();maths_.reset();tables_.reset();rows_.reset();cells_.reset();tex_.reset();mathCount_=tableCount_=rowCount_=cellCount_=texBytes_=0;count_=rubyCount_=transitionCount_=0;extended_=vertical_=false;source_=nullptr;bytes_=0;error_=Error::None;}
  Error error()const{return error_;}
  bool allocationFailed()const{return error_==Error::Allocation;}
  bool ready()const{return source_&&bytes_&&count_;}
  const char* text()const{return source_;}
  const char* source()const{return source_;}
  size_t size()const{return bytes_;}
  size_t sourceSize()const{return bytes_;}
  size_t runCount()const{return count_;}
  const markdown::Run& run(size_t index)const{return runs_[index];}
  bool hasVertical()const{return vertical_;}
  bool verticalAt(size_t at)const{const auto i=runAt(at);return i<count_&&modes_&&(modes_[i]&1);}
  bool combineAt(size_t at)const{const auto i=runAt(at);return i<count_&&modes_&&(modes_[i]&2);}
  size_t modeEnd(size_t at)const{if(!vertical_)return bytes_;const auto* first=transitions_.get();if(!transitionCount_)return bytes_;const auto* end=first+transitionCount_;const auto* found=std::upper_bound(first,end,at);return found==end?bytes_:*found;}
  size_t mathCount()const{return mathCount_;}size_t tableCount()const{return tableCount_;}
  const epub_objects::Math& math(size_t i)const{return maths_[i];}const char* mathText(size_t i)const{return tex_.get()+maths_[i].texOffset;}
  const epub_objects::Table& table(size_t i)const{return tables_[i];}const epub_objects::Row& row(size_t i)const{return rows_[i];}const epub_objects::Cell& cell(size_t i)const{return cells_[i];}
  size_t mathAt(size_t at)const{size_t first=0,last=mathCount_;while(first<last){size_t mid=(first+last)/2;if(maths_[mid].end<=at)first=mid+1;else last=mid;}return first<mathCount_&&maths_[first].begin<=at?first:mathCount_;}
  size_t tableAt(size_t at)const{size_t first=0,last=tableCount_;while(first<last){size_t mid=(first+last)/2;if(tables_[mid].end<=at)first=mid+1;else last=mid;}return first<tableCount_&&tables_[first].begin<=at?first:tableCount_;}
  size_t nextTable(size_t at)const{for(size_t i=0;i<tableCount_;++i)if(tables_[i].begin>=at)return tables_[i].begin;return bytes_;}
  size_t rubyCount()const{return rubyCount_;}
  const Ruby& ruby(size_t i)const{return rubies_[i];}
  const Ruby* rubyAt(size_t at)const{size_t l=0,r=rubyCount_;while(l<r){size_t m=(l+r)/2;if(rubies_[m].end<=at)l=m+1;else r=m;}return l<rubyCount_&&rubies_[l].begin<=at?rubies_.get()+l:nullptr;}
  size_t sourceOffset(size_t at)const{return std::min(at,bytes_);}
  size_t renderOffset(size_t at)const{return std::min(at,bytes_);}
  size_t runAt(size_t at)const{
    size_t first=0,last=count_;
    while(first<last){size_t mid=first+(last-first)/2;if(runs_[mid].end<=at)first=mid+1;else last=mid;}
    return first;
  }
  bool assign(const epub::structure::Collector& collector,const char* text,size_t bytes){
    clear();if(!collector.ready()){
      const auto status=collector.status();
      return fail(status==epub::structure::Status::Allocation?Error::Allocation:
        status==epub::structure::Status::Budget?Error::Budget:Error::Unavailable);
    }
    if(!validText(text,bytes))return fail();
    const size_t capacity=std::min(size_t(MAX_RUNS),collector.recordCount()*2+1);
    runs_.reset(new(std::nothrow) markdown::Run[capacity]);if(!runs_)return fail(Error::Allocation);
    source_=text;bytes_=bytes;uint32_t cursor=0;
    modes_.reset(new(std::nothrow) uint8_t[capacity]{});if(!modes_)return fail(Error::Allocation);
    const auto* records=collector.records();
    for(size_t i=0;i<collector.recordCount();++i){
      const auto& record=records[i];using K=epub::structure::Kind;
      if(record.kind!=K::Text&&record.kind!=K::Break&&record.kind!=K::Title)continue;
      if(record.begin==record.end)continue;
      if(record.begin<cursor||record.end>bytes_)return fail();
      const uint8_t mode=((record.flags&epub::structure::VerticalRL)?1:0)|((record.flags&epub::structure::Combine)?2:0);
      markdown::Run style;style.language=text_language::fromTag(collector.language(record.language));
      if(record.flags&epub::structure::Strong)style.flags|=markdown::Strong;
      if(record.flags&epub::structure::Emphasis)style.flags|=markdown::Emphasis;
      if(record.flags&epub::structure::Code)style.flags|=markdown::Code;
      if(record.flags&epub::structure::Preserve)style.flags|=markdown::Preformatted;
      uint32_t parent=uint32_t(i);unsigned visited=0;
      while(parent!=epub::structure::NONE){
        if(parent>i||++visited>130)return fail();
        const auto& block=records[parent];
        if(block.kind==K::Heading||block.kind==K::Title){style.flags|=markdown::Heading;style.headingLevel=block.headingLevel?block.headingLevel:1;}
        if(block.kind==K::Quote){style.flags|=markdown::Quote;style.indent=std::min<unsigned>(8,style.indent+1);}
        if(block.kind==K::ListItem||block.kind==K::DefinitionDetail)style.indent=std::min<unsigned>(8,style.indent+1);
        if(block.kind==K::Preformatted)style.flags|=markdown::Code|markdown::Preformatted;
        parent=block.parent;
      }
      markdown::Run gap=count_?runs_[count_-1]:markdown::Run{};
      if(cursor<record.begin&&!append(cursor,record.begin,gap,capacity,count_?modes_[count_-1]:0))return fail(Error::Budget);
      if(!append(record.begin,record.end,style,capacity,mode))return fail(Error::Budget);cursor=record.end;
    }
    markdown::Run tail=count_?runs_[count_-1]:markdown::Run{};
    if(cursor<bytes_&&!append(cursor,uint32_t(bytes_),tail,capacity,count_?modes_[count_-1]:0))return fail(Error::Budget);
    if(!prepareTransitions())return fail(Error::Allocation);
    size_t candidates=0;for(size_t i=0;i<collector.recordCount();++i)if(records[i].kind==epub::structure::Kind::Ruby&&records[i].flags&epub::structure::VerticalRL)++candidates;
    if(candidates){if(candidates>MAX_RUBIES)return fail(Error::Budget);rubies_.reset(new(std::nothrow) Ruby[candidates]);if(!rubies_)return fail(Error::Allocation);
      for(size_t i=0;i<collector.recordCount();++i){const auto& r=records[i];if(r.kind!=epub::structure::Kind::Ruby||!(r.flags&epub::structure::VerticalRL))continue;
        Ruby ruby{r.begin,r.end,0,0,r.end};unsigned notes=0;bool invalid=false;
        uint32_t ancestor=r.parent;while(ancestor!=epub::structure::NONE){if(records[ancestor].kind==epub::structure::Kind::Ruby)invalid=true;ancestor=records[ancestor].parent;}
        for(size_t j=i+1;j<collector.recordCount()&&records[j].begin<r.end;++j){const auto& child=records[j];
          if(child.kind==epub::structure::Kind::Ruby){invalid=true;break;}
          if(child.kind==epub::structure::Kind::RubyFallback)ruby.baseEnd=std::min(ruby.baseEnd,child.begin);
          if(child.kind==epub::structure::Kind::RubyText){++notes;ruby.baseEnd=std::min(ruby.baseEnd,child.begin);ruby.noteBegin=child.begin;ruby.noteEnd=child.end;}
        }
        // Only one terminal annotation is admitted. Interleaved base/rt pairs,
        // nested ruby and trailing base text remain complete canonical prose.
        if(!invalid&&notes==1)for(size_t j=i+1;j<collector.recordCount()&&records[j].begin<r.end;++j){const auto& child=records[j];
          if(child.kind!=epub::structure::Kind::Text||child.begin<ruby.baseEnd||child.begin>=ruby.noteBegin&&child.end<=ruby.noteEnd)continue;
          uint32_t parent=child.parent;bool fallback=false;while(parent!=epub::structure::NONE&&parent!=i){if(records[parent].kind==epub::structure::Kind::RubyFallback)fallback=true;parent=records[parent].parent;}if(!fallback)invalid=true;
        }
        if(!invalid&&notes==1&&modeEnd(ruby.begin)>=ruby.end&&validRuby(ruby)&&(rubyCount_==0||rubies_[rubyCount_-1].end<=ruby.begin))rubies_[rubyCount_++]=ruby;
      }
    }
    if(!assignObjects(collector)||!overlayMath())return fail(Error::Allocation);
    extended_=vertical_||rubyCount_;if(!extended_&&!mathCount_&&!tableCount_)modes_.reset();return ready();
  }
  bool v3()const{return mathCount_||tableCount_;}
  size_t encodedSize()const{return ready()?(v3()?epub_metadata_limits::V3_HEADER_BYTES:extended_?V2_HEADER_BYTES:HEADER_BYTES)+count_*RECORD_BYTES+rubyCount_*RUBY_BYTES+mathCount_*16+tableCount_*16+rowCount_*16+cellCount_*8+texBytes_:0;}
  bool encodeTo(uint8_t* out,size_t bytes)const{
    if(!out||bytes!=encodedSize()||!ready())return false;
    std::memset(out,0,bytes);std::memcpy(out,v3()?"PDS3":extended_?"PDS2":"PDS1",4);put(out+4,v3()?3:extended_?2:1);put(out+8,uint32_t(count_));put(out+12,uint32_t(bytes_));
    const size_t header=v3()?epub_metadata_limits::V3_HEADER_BYTES:extended_?V2_HEADER_BYTES:HEADER_BYTES;if(extended_||v3())put(out+16,uint32_t(rubyCount_));
    if(v3()){put(out+20,uint32_t(mathCount_));put(out+24,uint32_t(tableCount_));put(out+28,uint32_t(rowCount_));put(out+32,uint32_t(cellCount_));put(out+36,uint32_t(texBytes_));}
    for(size_t i=0;i<count_;++i){const auto& r=runs_[i];auto* p=out+header+i*RECORD_BYTES;
      put(p,r.begin);put(p+4,r.end);p[8]=uint8_t(r.flags);p[9]=uint8_t(r.flags>>8);
      p[10]=r.indent;p[11]=r.headingLevel;p[12]=uint8_t(r.language);if(extended_||v3())p[13]=modes_[i];if(v3())p[14]=uint8_t(r.kind);
    }
    for(size_t i=0;i<rubyCount_;++i){auto* p=out+header+count_*RECORD_BYTES+i*RUBY_BYTES;const auto& r=rubies_[i];put(p,r.begin);put(p+4,r.baseEnd);put(p+8,r.noteBegin);put(p+12,r.noteEnd);put(p+16,r.end);}
    size_t offset=header+count_*16+rubyCount_*20;
    for(size_t i=0;i<mathCount_;++i){const auto& m=maths_[i];auto* p=out+offset;put(p,m.begin);put(p+4,m.end);put(p+8,m.texOffset);p[12]=uint8_t(m.texBytes);p[13]=uint8_t(m.texBytes>>8);p[14]=uint8_t(m.flags);p[15]=uint8_t(m.flags>>8);offset+=16;}
    for(size_t i=0;i<tableCount_;++i){const auto& t=tables_[i];auto* p=out+offset;put(p,t.begin);put(p+4,t.end);put(p+8,t.firstRow);p[12]=uint8_t(t.rows);p[13]=uint8_t(t.rows>>8);p[14]=uint8_t(t.columns);p[15]=uint8_t(t.columns>>8);offset+=16;}
    for(size_t i=0;i<rowCount_;++i){const auto& r=rows_[i];auto* p=out+offset;put(p,r.begin);put(p+4,r.end);put(p+8,r.firstCell);p[12]=uint8_t(r.cells);p[13]=uint8_t(r.cells>>8);p[14]=uint8_t(r.flags);p[15]=uint8_t(r.flags>>8);offset+=16;}
    for(size_t i=0;i<cellCount_;++i){auto* p=out+offset;put(p,cells_[i].begin);put(p+4,cells_[i].end);offset+=8;}if(texBytes_)std::memcpy(out+offset,tex_.get(),texBytes_);return true;
  }
  bool encode(std::vector<uint8_t>& out)const{
    out.clear();if(!ready())return false;out.resize(encodedSize());return encodeTo(out.data(),out.size());
  }
  bool decode(const uint8_t* data,size_t size,const char* text,size_t bytes,bool verifyText=true){
    clear();if(!data||size<HEADER_BYTES||size>MAX_BYTES||!text||!bytes||bytes>4u*1024*1024||(verifyText&&!validText(text,bytes))||get(data+12)!=bytes)return fail();
    const bool v3=!std::memcmp(data,"PDS3",4)&&get(data+4)==3;
    const bool v2=!std::memcmp(data,"PDS2",4)&&get(data+4)==2;
    if(!v2&&!v3&&(std::memcmp(data,"PDS1",4)||get(data+4)!=1))return fail();
    const size_t header=v3?epub_metadata_limits::V3_HEADER_BYTES:v2?V2_HEADER_BYTES:HEADER_BYTES;if(size<header)return fail();
    const size_t count=get(data+8),rubyCount=v2||v3?get(data+16):0;
    const size_t mathCount=v3?get(data+20):0,tableCount=v3?get(data+24):0,rowCount=v3?get(data+28):0,cellCount=v3?get(data+32):0,texBytes=v3?get(data+36):0;
    if(!count||count>MAX_RUNS||rubyCount>MAX_RUBIES||(v2&&get(data+20))||mathCount>epub_objects::MAX_MATH||tableCount>epub_objects::MAX_TABLES||rowCount>epub_objects::MAX_ROWS||cellCount>epub_objects::MAX_CELLS||texBytes>epub_objects::MAX_TEX_BYTES||size!=header+count*RECORD_BYTES+rubyCount*RUBY_BYTES+mathCount*16+tableCount*16+rowCount*16+cellCount*8+texBytes)return fail();
    runs_.reset(new(std::nothrow) markdown::Run[count]);if(!runs_)return fail(Error::Allocation);
    source_=text;bytes_=bytes;uint32_t next=0;
    if(v2||v3){modes_.reset(new(std::nothrow) uint8_t[count]{});if(!modes_)return fail(Error::Allocation);}
    for(size_t i=0;i<count;++i){const auto* p=data+header+i*RECORD_BYTES;markdown::Run r;
      r.begin=get(p);r.end=get(p+4);r.sourceBegin=r.begin;r.sourceEnd=r.end;
      r.flags=uint16_t(p[8])|(uint16_t(p[9])<<8);r.indent=p[10];r.headingLevel=p[11];r.kind=v3?markdown::Kind(p[14]):markdown::Kind::Text;r.formulaBegin=r.begin;r.formulaEnd=r.end;r.language=text_language::Language(p[12]);
      constexpr uint16_t allowed=markdown::Strong|markdown::Emphasis|markdown::Code|markdown::Heading|markdown::Quote|markdown::Preformatted;
      if(r.begin!=next||r.end<=r.begin||r.end>bytes||r.flags&~allowed||r.indent>8||r.headingLevel>6||
        bool(r.flags&markdown::Heading)!=(r.headingLevel!=0)||!text_language::valid(r.language)||((v2||v3)?(p[13]&~3u):p[13])||(v3?(p[14]>uint8_t(markdown::Kind::MathBlock)):p[14])||p[15]||
        (verifyText&&(continuation(text[r.begin])||(r.end<bytes&&continuation(text[r.end])))))return fail();
      if(v2||v3){modes_[count_]=p[13];vertical_|=bool(p[13]&1);}
      runs_[count_++]=r;next=r.end;
    }
    if(next!=bytes)return fail();
    if(!prepareTransitions())return fail(Error::Allocation);
    if(rubyCount){rubies_.reset(new(std::nothrow) Ruby[rubyCount]);if(!rubies_)return fail(Error::Allocation);
      for(size_t i=0;i<rubyCount;++i){const auto* p=data+header+count*RECORD_BYTES+i*RUBY_BYTES;Ruby r{get(p),get(p+4),get(p+8),get(p+12),get(p+16)};
        if(!(verifyText?validRuby(r):validRubyBounds(r))||!verticalAt(r.begin)||modeEnd(r.begin)<r.end||(i&&rubies_[i-1].end>r.begin))return fail();rubies_[rubyCount_++]=r;}
    }
    if(v3&&!decodeObjects(data+header+count*16+rubyCount*20,mathCount,tableCount,rowCount,cellCount,texBytes,verifyText))return fail(error_==Error::Allocation?Error::Allocation:Error::Invalid);
    extended_=v2||vertical_||rubyCount_;return true;
  }
  // Only inspect authenticated, resident bytes. Any intersecting ruby must be
  // completely pinned before its canonical group is painted or measured.
  bool validateSpan(size_t begin,size_t end)const{
    if(!ready()||begin>end||end>bytes_)return false;
    for(size_t i=runAt(begin);i<count_&&runs_[i].begin<end;++i){const auto& r=runs_[i];
      if((r.begin>=begin&&continuation(source_[r.begin]))||(r.end<end&&continuation(source_[r.end])))return false;
    }
    for(size_t i=0;i<rubyCount_;++i){const auto& r=rubies_[i];if(r.end<=begin)continue;if(r.begin>=end)break;
      if(r.begin<begin||r.end>end||(r.end==end&&end<bytes_)||!validRuby(r))return false;
    }
    for(size_t i=0;i<mathCount_;++i){const auto& m=maths_[i];if(m.end<=begin)continue;if(m.begin>=end)break;if(m.begin<begin||m.end>end||(m.end==end&&end<bytes_)||!boundary(m.begin)||!boundary(m.end))return false;}
    for(size_t i=0;i<rowCount_;++i){const auto& r=rows_[i];if(r.end<=begin)continue;if(r.begin>=end)break;if(r.begin<begin||r.end>end||(r.end==end&&end<bytes_)||!boundary(r.begin)||!boundary(r.end))return false;for(size_t j=r.firstCell;j<r.firstCell+r.cells;++j)if(!boundary(cells_[j].begin)||!boundary(cells_[j].end))return false;}
    return true;
  }
  static bool validText(const char* text,size_t bytes){
    if(!text||!bytes||bytes>4u*1024*1024)return false;
    for(size_t at=0;at<bytes;){uint8_t c=uint8_t(text[at++]);if(c<128){if(!c)return false;continue;}
      unsigned n;uint32_t cp,min;
      if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=128;}else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=2048;}
      else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=65536;}else return false;
      if(n>bytes-at)return false;while(n--){c=uint8_t(text[at++]);if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
      if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
    }return true;
  }
 private:
  static bool continuation(char c){return (uint8_t(c)&0xc0)==0x80;}
  static uint32_t get(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  bool fail(Error reason=Error::Invalid){clear();error_=reason;return false;}
  bool append(uint32_t begin,uint32_t end,markdown::Run style,size_t capacity,uint8_t mode){
    if(count_){auto& last=runs_[count_-1];if(last.end==begin&&last.flags==style.flags&&last.indent==style.indent&&
      last.headingLevel==style.headingLevel&&last.language==style.language&&modes_[count_-1]==mode){last.end=end;last.sourceEnd=end;return true;}}
    if(count_>=capacity)return false;style.begin=style.sourceBegin=begin;style.end=style.sourceEnd=end;modes_[count_]=mode;vertical_|=bool(mode&1);runs_[count_++]=style;return true;
  }
  bool prepareTransitions(){
    if(!vertical_)return true;
    size_t count=0;for(size_t i=1;i<count_;++i)if((modes_[i]&1)!=(modes_[i-1]&1))++count;
    if(!count)return true;transitions_.reset(new(std::nothrow) uint32_t[count]);if(!transitions_)return false;
    for(size_t i=1;i<count_;++i)if((modes_[i]&1)!=(modes_[i-1]&1))transitions_[transitionCount_++]=runs_[i].begin;return true;
  }
  size_t characters(uint32_t begin,uint32_t end)const{size_t count=0;for(uint32_t at=begin;at<end;++at){if(source_[at]=='\n'||source_[at]=='\r')return SIZE_MAX;if(!continuation(source_[at]))++count;}return count;}
  bool validRubyBounds(const Ruby& r)const{return r.begin<r.baseEnd&&r.baseEnd<=r.noteBegin&&r.noteBegin<r.noteEnd&&r.noteEnd<=r.end&&r.end<=bytes_;}
  bool validRuby(const Ruby& r)const{
    if(!validRubyBounds(r))return false;
    for(auto at:{r.begin,r.baseEnd,r.noteBegin,r.noteEnd,r.end})if(at<bytes_&&continuation(source_[at]))return false;
    if(characters(r.begin,r.baseEnd)>8||characters(r.noteBegin,r.noteEnd)>16)return false;
    for(size_t at=r.noteBegin;at<r.noteEnd;){uint8_t first=uint8_t(source_[at]);size_t bytes=first<128?1:first<224?2:first<240?3:4;uint32_t cp=first&((1u<<(7-bytes))-1);if(bytes==1)cp=first;for(size_t i=1;i<bytes;++i)cp=(cp<<6)|(uint8_t(source_[at+i])&63);
      if(!((cp>=32&&cp<0x100)||(cp>=0x3000&&cp<0x3100)))return false;at+=bytes;}
    return true;
  }
  bool boundary(size_t at)const{return at==bytes_||(at<bytes_&&!continuation(source_[at]));}
  bool assignObjects(const epub::structure::Collector& collector);
  bool overlayMath();
  bool decodeObjects(const uint8_t* data,size_t mathCount,size_t tableCount,size_t rowCount,size_t cellCount,size_t texBytes,bool verifyText);
  Error error_=Error::None;
  std::unique_ptr<markdown::Run[]> runs_;
  std::unique_ptr<uint8_t[]> modes_;std::unique_ptr<uint32_t[]> transitions_;std::unique_ptr<Ruby[]> rubies_;
  size_t rubyCount_=0,transitionCount_=0;bool extended_=false,vertical_=false;
  std::unique_ptr<epub_objects::Math[]> maths_;std::unique_ptr<epub_objects::Table[]> tables_;std::unique_ptr<epub_objects::Row[]> rows_;std::unique_ptr<epub_objects::Cell[]> cells_;std::unique_ptr<char[]> tex_;
  size_t mathCount_=0,tableCount_=0,rowCount_=0,cellCount_=0,texBytes_=0;
  const char* source_=nullptr;size_t bytes_=0,count_=0;
};
} // namespace epub_document

namespace epub_document {
inline bool Document::assignObjects(const epub::structure::Collector& collector){
  const size_t candidates=collector.mathCount();
  if(candidates){size_t texCapacity=0;for(size_t i=0;i<candidates;++i)texCapacity+=collector.math(i).texBytes;maths_.reset(new(std::nothrow) epub_objects::Math[candidates]);tex_.reset(new(std::nothrow) char[texCapacity]);if(!maths_||!tex_)return false;
    for(size_t i=0;i<candidates;++i){auto m=collector.math(i);if(m.begin>=m.end||m.end>bytes_||verticalAt(m.begin)||modeEnd(m.begin)<m.end||(mathCount_&&maths_[mathCount_-1].end>m.begin))continue;
      m.texOffset=uint32_t(texBytes_);std::memcpy(tex_.get()+texBytes_,collector.mathText(i),m.texBytes);texBytes_+=m.texBytes;maths_[mathCount_++]=m;
    }
  }
  using K=epub::structure::Kind;const auto* records=collector.records();size_t tableCandidates=0;
  for(size_t i=0;i<collector.recordCount();++i)if(records[i].kind==K::Table)++tableCandidates;
  if(!tableCandidates)return true;
  tables_.reset(new(std::nothrow) epub_objects::Table[epub_objects::MAX_TABLES]);rows_.reset(new(std::nothrow) epub_objects::Row[epub_objects::MAX_ROWS]);cells_.reset(new(std::nothrow) epub_objects::Cell[epub_objects::MAX_CELLS]);if(!tables_||!rows_||!cells_)return false;
  for(size_t i=0;i<collector.recordCount()&&tableCount_<epub_objects::MAX_TABLES;++i){const auto& r=records[i];if(r.kind!=K::Table||r.begin>=r.end||r.end>bytes_||verticalAt(r.begin)||modeEnd(r.begin)<r.end)continue;
    bool invalid=false;uint32_t parent=r.parent;while(parent!=epub::structure::NONE){if(records[parent].kind==K::Table)invalid=true;parent=records[parent].parent;}
    const size_t firstRow=rowCount_,firstCell=cellCount_;unsigned columns=0;
    for(size_t j=i+1;j<collector.recordCount()&&records[j].begin<r.end;++j){const auto& child=records[j];if(child.kind==K::Table){invalid=true;break;}if(child.kind!=K::TableRow)continue;
      if(rowCount_==epub_objects::MAX_ROWS||child.end<=child.begin||child.end-child.begin>epub_objects::MAX_ROW_SOURCE){invalid=true;break;}
      epub_objects::Row row{child.begin,child.end,uint32_t(cellCount_),0,0};
      for(size_t k=j+1;k<collector.recordCount()&&records[k].begin<=child.end;++k){const auto& c=records[k];if(c.kind!=K::TableCell||c.parent!=j)continue;
        if(cellCount_==epub_objects::MAX_CELLS||c.flags&epub::structure::UnsupportedLayout||c.begin<child.begin||c.end>child.end||c.end<c.begin){invalid=true;break;}
        if(c.flags&epub::structure::TableHeader)row.flags=1;cells_[cellCount_++]={c.begin,c.end};++row.cells;
      }
      if(row.cells<2||row.cells>4||(columns&&columns!=row.cells)){invalid=true;break;}columns=row.cells;rows_[rowCount_++]=row;
    }
    if(rowCount_==firstRow)invalid=true;
    for(size_t j=i+1;!invalid&&j<collector.recordCount()&&records[j].begin<r.end;++j){const auto& child=records[j];if((child.kind!=K::Text&&child.kind!=K::Break)||child.begin==child.end)continue;
      bool contained=false;for(size_t k=firstCell;k<cellCount_;++k)if(child.begin>=cells_[k].begin&&child.end<=cells_[k].end){contained=true;break;}if(!contained)invalid=true;
    }
    for(size_t j=0;j<mathCount_;++j)if(maths_[j].begin<r.end&&maths_[j].end>r.begin)invalid=true;
    if(invalid){rowCount_=firstRow;cellCount_=firstCell;continue;}
    tables_[tableCount_++]={r.begin,r.end,uint32_t(firstRow),uint16_t(rowCount_-firstRow),uint16_t(columns)};
  }return true;
}
inline bool Document::overlayMath(){
  if(!mathCount_)return true;const size_t capacity=std::min(MAX_RUNS,count_+mathCount_*2);
  std::unique_ptr<markdown::Run[]> runs(new(std::nothrow) markdown::Run[capacity]);std::unique_ptr<uint8_t[]> modes(new(std::nothrow) uint8_t[capacity]);if(!runs||!modes)return false;
  size_t cursor=0,runIndex=0,mathIndex=0,count=0;
  while(cursor<bytes_){while(runIndex<count_&&runs_[runIndex].end<=cursor)++runIndex;if(runIndex==count_||count==capacity)return false;
    auto run=runs_[runIndex];const uint8_t mode=modes_?modes_[runIndex]:0;run.begin=run.sourceBegin=uint32_t(cursor);
    if(mathIndex<mathCount_&&maths_[mathIndex].begin==cursor){const auto& m=maths_[mathIndex++];run.end=run.sourceEnd=m.end;run.kind=(m.flags&epub_objects::MATH_DISPLAY)?markdown::Kind::MathBlock:markdown::Kind::MathInline;run.formulaBegin=m.begin;run.formulaEnd=m.end;}
    else {run.end=run.sourceEnd=uint32_t(mathIndex<mathCount_?std::min<size_t>(run.end,maths_[mathIndex].begin):run.end);}
    if(run.end<=cursor)return false;runs[count]=run;modes[count++]=mode;cursor=run.end;
  }
  runs_=std::move(runs);modes_=std::move(modes);count_=count;transitions_.reset();transitionCount_=0;return prepareTransitions();
}
inline bool Document::decodeObjects(const uint8_t* data,size_t mathCount,size_t tableCount,size_t rowCount,size_t cellCount,size_t texBytes,bool verifyText){
  auto word16=[](const uint8_t* p)->uint16_t{return uint16_t(uint16_t(p[0])|(uint16_t(p[1])<<8));};
  if(mathCount){maths_.reset(new(std::nothrow) epub_objects::Math[mathCount]);if(!maths_){error_=Error::Allocation;return false;}}
  if(tableCount){tables_.reset(new(std::nothrow) epub_objects::Table[tableCount]);if(!tables_){error_=Error::Allocation;return false;}}
  if(rowCount){rows_.reset(new(std::nothrow) epub_objects::Row[rowCount]);if(!rows_){error_=Error::Allocation;return false;}}
  if(cellCount){cells_.reset(new(std::nothrow) epub_objects::Cell[cellCount]);if(!cells_){error_=Error::Allocation;return false;}}
  if(texBytes){tex_.reset(new(std::nothrow) char[texBytes]);if(!tex_){error_=Error::Allocation;return false;}}
  size_t offset=0,texEnd=0;
  for(size_t i=0;i<mathCount;++i){const auto* p=data+offset;offset+=16;epub_objects::Math m{get(p),get(p+4),get(p+8),word16(p+12),word16(p+14)};
    if(m.begin>=m.end||m.end>bytes_||m.end-m.begin>epub_objects::MAX_MATH_SOURCE||m.texOffset!=texEnd||!m.texBytes||m.texBytes>4096||m.texBytes>texBytes-texEnd||m.flags&~epub_objects::MATH_DISPLAY||(i&&maths_[i-1].end>m.begin)||verticalAt(m.begin)||modeEnd(m.begin)<m.end||(verifyText&&(!boundary(m.begin)||!boundary(m.end))))return false;
    texEnd+=m.texBytes;maths_[mathCount_++]=m;
  }
  if(texEnd!=texBytes)return false;
  for(size_t i=0;i<tableCount;++i){const auto* p=data+offset;offset+=16;epub_objects::Table t{get(p),get(p+4),get(p+8),word16(p+12),word16(p+14)};
    if(t.begin>=t.end||t.end>bytes_||!t.rows||t.firstRow>rowCount||t.rows>rowCount-t.firstRow||t.columns<2||t.columns>4||(i&&tables_[i-1].end>t.begin)||verticalAt(t.begin)||modeEnd(t.begin)<t.end||(verifyText&&(!boundary(t.begin)||!boundary(t.end))))return false;tables_[tableCount_++]=t;
  }
  for(size_t i=0;i<rowCount;++i){const auto* p=data+offset;offset+=16;epub_objects::Row r{get(p),get(p+4),get(p+8),word16(p+12),word16(p+14)};
    if(r.begin>=r.end||r.end>bytes_||r.end-r.begin>epub_objects::MAX_ROW_SOURCE||r.firstCell>cellCount||r.cells>cellCount-r.firstCell||r.cells<2||r.cells>4||r.flags&~1u||(i&&rows_[i-1].end>r.begin)||(verifyText&&(!boundary(r.begin)||!boundary(r.end))))return false;rows_[rowCount_++]=r;
  }
  for(size_t i=0;i<cellCount;++i){const auto* p=data+offset;offset+=8;epub_objects::Cell c{get(p),get(p+4)};if(c.begin>c.end||c.end>bytes_||(i&&cells_[i-1].end>c.begin)||(verifyText&&(!boundary(c.begin)||!boundary(c.end))))return false;cells_[cellCount_++]=c;}
  size_t nextRow=0,nextCell=0;
  for(size_t i=0;i<tableCount_;++i){const auto& t=tables_[i];if(t.firstRow!=nextRow)return false;for(size_t j=t.firstRow;j<t.firstRow+t.rows;++j){const auto& r=rows_[j];if(r.begin<t.begin||r.end>t.end||r.cells!=t.columns||r.firstCell!=nextCell)return false;for(size_t k=r.firstCell;k<r.firstCell+r.cells;++k)if(cells_[k].begin<r.begin||cells_[k].end>r.end)return false;nextCell+=r.cells;}nextRow+=t.rows;}
  if(nextRow!=rowCount_||nextCell!=cellCount_)return false;
  for(size_t i=0;i<mathCount_;++i)for(size_t j=0;j<tableCount_;++j)if(maths_[i].begin<tables_[j].end&&maths_[i].end>tables_[j].begin)return false;
  if(texBytes){std::memcpy(tex_.get(),data+offset,texBytes);if(!validText(tex_.get(),texBytes))return false;}texBytes_=texBytes;for(size_t i=0;i<mathCount_;++i)if(!validText(mathText(i),maths_[i].texBytes))return false;
  size_t observed=0;for(size_t i=0;i<count_;++i){const auto& r=runs_[i];if(r.kind==markdown::Kind::Text)continue;if(observed==mathCount_||r.begin!=maths_[observed].begin||r.end!=maths_[observed].end||bool(r.kind==markdown::Kind::MathBlock)!=bool(maths_[observed].flags&epub_objects::MATH_DISPLAY))return false;++observed;}
  return observed==mathCount_;
}
} // namespace epub_document
