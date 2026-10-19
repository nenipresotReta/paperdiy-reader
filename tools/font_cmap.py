"""Read Unicode coverage from an OpenType cmap (formats 4, 12, 13)."""
import struct
def u16(d,p):return struct.unpack_from('>H',d,p)[0]
def u32(d,p):return struct.unpack_from('>I',d,p)[0]
def cmap(d,base):
 tables={d[p:p+4]:u32(d,p+8) for p in range(base+12,base+12+16*u16(d,base+4),16)}
 t=tables.get(b'cmap');out=set()
 if t is None:return out
 for i in range(u16(d,t+2)):
  p=t+4+i*8;platform=u16(d,p);enc=u16(d,p+2)
  if platform!=0 and (platform!=3 or enc not in (1,10)):continue
  s=t+u32(d,p+4);fmt=u16(d,s)
  if fmt==4:
   n=u16(d,s+6)//2;ends=s+14;starts=ends+2*n+2;deltas=starts+2*n;ranges=deltas+2*n
   for j in range(n):
    delta=u16(d,deltas+2*j);roff=u16(d,ranges+2*j)
    for cp in range(u16(d,starts+2*j),u16(d,ends+2*j)+1):
     if cp==65535:continue
     glyph=(cp+delta)&65535 if not roff else u16(d,ranges+2*j+roff+2*(cp-u16(d,starts+2*j)))
     if roff and glyph:glyph=(glyph+delta)&65535
     if glyph:out.add(cp)
  elif fmt in (12,13):
   for j in range(u32(d,s+12)):
    p=s+16+j*12;a,b,g=struct.unpack_from('>III',d,p)
    if b>0x10ffff:raise ValueError('invalid cmap range')
    if fmt==13 and not g:continue
    out.update(range(a+(g==0),b+1))
 return out
