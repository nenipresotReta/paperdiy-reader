#pragma once
#include "sd_fixture_core.h"
#include "sd_audit_file.h"
#include <cerrno>
#include <fcntl.h>
namespace sd_fixture {
struct File : sd_audit::File {
  int writer=-1;
  bool (*mutate)(void*);
  File(const char* path,bool (*allowed)(void*),void (*cooperate)(void*),void* context,bool (*mutation)(void*)=nullptr)
    :sd_audit::File(path,allowed,cooperate,context),mutate(mutation){}
  ~File(){if(writer>=0)::close(writer);}
  bool prepareMutation(){return mutate?mutate(context):guard();}
  Create createExclusive(){
    writer=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    return writer>=0?Create::Created:errno==EEXIST?Create::Exists:Create::Failed;
  }
  size_t write(const uint8_t* p,size_t bytes){const ssize_t n=::write(writer,p,bytes);return n>0?size_t(n):0;}
  bool sync(){return writer>=0&&!::fsync(writer);}
  bool closeWrite(){if(writer<0)return false;const int fd=writer;writer=-1;return !::close(fd);}
};
} // namespace sd_fixture
