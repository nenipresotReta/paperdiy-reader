#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include "lan_batch.h"

namespace {
using delivery_store::Kind;
bool digest(const uint8_t* bytes,size_t length,uint8_t out[32]){return SHA256(bytes,length,out)!=nullptr;}
delivery_store::Metadata sample(Kind kind){
  delivery_store::Metadata m;m.requestId=std::string(32,'a');m.fingerprint=std::string(64,'b');m.sha256=std::string(64,'c');
  m.kind=kind;m.name=kind==Kind::Push?"Push.md":"\xe6\x9b\xb8 book.epub";m.title="Synthetic \xe9\xa1\x8c";m.size=4096;m.baseRevision=kind==Kind::Push?7:0;
  return m;
}
bool parses(const std::string& packed){delivery_store::Metadata m;return lan_batch::parseEntry(packed,m);}
std::string field(const std::string& value){return std::to_string(value.size())+":"+value;}
std::string packed(const std::string& request,const std::string& fp,const std::string& kind,const std::string& name,
                   const std::string& title,const std::string& size,const std::string& sha,const std::string& base){
  return field(request)+field(fp)+field(kind)+field(name)+field(title)+field(size)+field(sha)+field(base);
}

void roundTrip(){
  for(const auto kind:{Kind::Push,Kind::File}){
    const auto m=sample(kind);const auto text=lan_batch::packEntry(m);
    delivery_store::Metadata parsed;assert(lan_batch::parseEntry(text,parsed));
    assert(parsed.requestId==m.requestId&&parsed.fingerprint==m.fingerprint&&parsed.kind==m.kind&&parsed.name==m.name);
    assert(parsed.title==m.title&&parsed.size==m.size&&parsed.sha256==m.sha256&&parsed.baseRevision==m.baseRevision);
    assert(parsed.clientId.empty()&&lan_batch::packEntry(parsed)==text);
  }
  // Shared with tests/lan_client.py (test_packed_entry_vector_matches_firmware).
  const std::string sender="32:"+std::string(32,'a')+"64:"+std::string(64,'b')+"4:file13:\xe6\x9b\xb8 book.epub13:Synthetic \xe9\xa1\x8c""4:409664:"+std::string(64,'c')+"0:";
  assert(lan_batch::packEntry(sample(Kind::File))==sender);
}
void rejections(){
  const std::string id(32,'a'),fp(64,'b'),sha(64,'c');
  assert(parses(packed(id,fp,"file","a.md","","1",sha,"")));
  assert(parses(packed(id,fp,"push","Push.md","","1",sha,"0")));
  assert(!parses(""));
  assert(!parses(packed(id,fp,"file","a.md","","1",sha,"")+"x"));             // trailing bytes
  assert(!parses(packed(id,fp,"file","a.md","","1",sha,"").substr(1)));       // truncated length
  assert(!parses(packed(id,fp,"other","a.md","","1",sha,"")));                // kind
  assert(!parses(packed(id,fp,"file","a.md","","1",sha,"0")));                // file revision
  assert(!parses(packed(id,fp,"push","Push.md","","1",sha,"")));              // push revision
  assert(!parses(packed(id,fp,"push","Push.md","","1",sha,"01")));            // non-canonical decimal
  assert(!parses(packed(id,fp,"file","a.md","","01",sha,"")));
  assert(!parses(packed(id,fp,"file","a.md","","4294967296",sha,"")));        // > uint32
  assert(!parses(packed(id,fp,"file","a.md","","-1",sha,"")));
  assert(!parses(packed(std::string(32,'A'),fp,"file","a.md","","1",sha,""))); // lowercase hex only
  assert(!parses(packed(id.substr(1),fp,"file","a.md","","1",sha,"")));
  assert(!parses(packed(id,fp,"file","dir/a.md","","1",sha,"")));             // filename rules
  assert(!parses(packed(id,fp,"file",".a.md","","1",sha,"")));
  assert(!parses(packed(id,fp,"file","a.md","\x01","1",sha,"")));             // control in title
  assert(!parses(packed(id,fp,"file","a\xc0\xaf.md","","1",sha,"")));         // overlong UTF-8
  assert(!parses(packed(id,fp,"file",std::string(97,'a'),"","1",sha,"")));    // field bound
  const std::string good=packed(id,fp,"file","a.md","","1",sha,"");
  assert(!parses("0"+good));                                                   // leading zero length
  assert(!parses("1000:"+good));                                               // four length digits
  std::string padded=good;padded.resize(lan_batch::PACKED_MAX+1,'a');assert(!parses(padded));
}
// Strict canonical parsing: any accepted mutation must re-pack to itself.
void fuzz(){
  std::mt19937 random(20261026);
  const std::string seeds[]={lan_batch::packEntry(sample(Kind::Push)),lan_batch::packEntry(sample(Kind::File))};
  const char alphabet[]="0123456789:abcdefpushfilePUSH.md|\xe6\x9b\xb8\x00\xff";
  unsigned accepted=0;
  for(unsigned iteration=0;iteration<200000;++iteration){
    std::string text=seeds[iteration&1];
    const unsigned mutations=1+random()%4;
    for(unsigned i=0;i<mutations;++i){
      const size_t at=text.empty()?0:random()%text.size();
      switch(random()%4){
        case 0:if(!text.empty())text[at]=alphabet[random()%(sizeof(alphabet)-1)];break;
        case 1:text.insert(at,1,alphabet[random()%(sizeof(alphabet)-1)]);break;
        case 2:if(!text.empty())text.erase(at,1+random()%3);break;
        default:text.insert(at,text.substr(random()%(text.size()+1)));break;
      }
    }
    delivery_store::Metadata m;
    if(lan_batch::parseEntry(text,m)){++accepted;assert(lan_batch::packEntry(m)==text);}
  }
  std::printf("packed-entry fuzz: 200000 cases, %u accepted, all canonical\n",accepted);
}
void cryptoVectors(){
  // RFC 4231 test case 2.
  uint8_t mac[32];assert(lan_batch::hmac(digest,reinterpret_cast<const uint8_t*>("Jefe"),4,"what do ya want for nothing?",mac));
  assert(delivery_store::hexadecimal(mac,32)=="5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Shared with tests/lan_client.py; computed independently with Python's hashlib/hmac.
  uint8_t f[32],ns[32],nd[32];std::memset(f,0x11,32);std::memset(ns,0x22,32);std::memset(nd,0x33,32);
  uint32_t sas;assert(lan_batch::sas(digest,f,ns,nd,"0123456789abcdef","Hermes \xe6\x9b\xb8",sas));
  assert(sas==506740&&lan_batch::sasText(sas)=="506 740");
  std::string proof,commit;
  assert(lan_batch::statusProof(digest,ns,"fedcba9876543210",proof)&&proof=="4b19bde7b8dd1e27aab707265889ba8c7a1b22e0bba59770d2f6dc3ebe6533e4");
  assert(lan_batch::commitment(digest,ns,commit)&&commit=="9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4");
  assert(lan_batch::sasText(7)=="000 007");
  assert(!lan_batch::sas(digest,f,ns,nd,"0123456789ABCDEF","x",sas));
  assert(!lan_batch::statusProof(digest,ns,"short",proof));
  assert(lan_batch::equalTime("abc","abc")&&!lan_batch::equalTime("abc","abd")&&!lan_batch::equalTime("abc","ab"));
}
}
int main(){roundTrip();rejections();fuzz();cryptoVectors();std::puts("LAN batch parser and pairing primitives: PASS");}
