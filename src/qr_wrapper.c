#include "qr_wrapper.h"
#include <lgfx/utility/lgfx_qrcode.h>

int qrMatrix(const char* text,uint8_t* out,int version){
  if(version<1||version>10||!text||!out)return 0;
  uint8_t bits[512];
  QRCode qr;
  if(lgfx_qrcode_initText(&qr,bits,(uint8_t)version,ECC_MEDIUM,text)!=0)return 0;
  if(qr.size==0||qr.size>64)return 0;
  for(int y=0;y<qr.size;++y)
    for(int x=0;x<qr.size;++x)
      out[y*qr.size+x]=lgfx_qrcode_getModule(&qr,x,y)?1:0;
  return qr.size;
}
