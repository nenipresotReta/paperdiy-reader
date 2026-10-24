#pragma once
constexpr uint8_t CARD_NONE=0,CARD_SDHC=3;
struct FakeSD {bool mounted=false;uint8_t cardType() const {return mounted?CARD_SDHC:CARD_NONE;}};
extern FakeSD SD;
