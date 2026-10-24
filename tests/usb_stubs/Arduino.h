#pragma once
#include <cstdint>
#include <cstring>
#define CONFIG_TINYUSB_MSC_ENABLED 1
constexpr int LOW=0,HIGH=1;
int digitalRead(int);
struct MockCdc {bool reboot=true;void enableReboot(bool value){reboot=value;}};
extern MockCdc Serial;
