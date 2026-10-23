#pragma once
#include "WiFi.h"
inline unsigned testDnsPolls=0;
struct DNSServer {
 bool running=false;
 void start(unsigned,const char*,IPAddress){running=true;}
 void stop(){running=false;}
 void processNextRequest(){++testDnsPolls;}
};
