"""Generate native monochrome reader fonts from a pinned, OFL-licensed source."""
from hashlib import sha256
from pathlib import Path
from urllib.request import urlopen
from PIL import Image, ImageDraw, ImageFont
from font_cmap import cmap

ROOT = Path(__file__).resolve().parents[1]
FONT_SHA = '2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca'
FONT_URL = ('https://raw.githubusercontent.com/notofonts/noto-cjk/'
            '9b0f1436e455d902de067a2501422e5dc71ad16b/'
            'Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Regular.otf')
font_path = ROOT / '.cache/NotoSerifCJKsc-Regular.otf'
font_path.parent.mkdir(parents=True, exist_ok=True)
if not font_path.exists():
    with urlopen(FONT_URL, timeout=120) as response:
        font_path.write_bytes(response.read())
if sha256(font_path.read_bytes()).hexdigest() != FONT_SHA:
    raise SystemExit('Font SHA-256 mismatch; remove the cached download and retry')

# A fixed public repertoire, independent of anyone's books or installed fonts.
codes = set(range(32, 0x300)) | set(range(0x370, 0x530))
codes |= set(range(0x2000, 0x2070)) | set(range(0x3000, 0x3040))
for encoding in ('gb2312', 'euc_jp'):
    for high in range(0xa1, 0xff):
        for low in range(0xa1, 0xff):
            try:
                text = bytes((high, low)).decode(encoding)
                if len(text) == 1:
                    codes.add(ord(text))
            except UnicodeDecodeError:
                pass

codes &= cmap(font_path.read_bytes(), 0)

lines = ['// Generated from Noto Serif CJK SC (SIL OFL 1.1). Do not edit.',
         '#pragma once', '#include <cstddef>', '#include <cstdint>',
         'struct NativeGlyph {uint32_t code; uint8_t advance; int8_t left; uint8_t width; uint32_t rows[28];};']
for size in (24, 28):
    font = ImageFont.truetype(str(font_path), size, layout_engine=ImageFont.Layout.BASIC)
    lines.append(f'static const NativeGlyph GLYPHS_{size}[]={{')
    for cp in sorted(codes):
        ch = chr(cp)
        box = font.getbbox(ch, anchor='ls')
        width, height = box[2]-box[0], box[3]-box[1]
        # Keep native geometry; never resample a bitmap or clip tall accents.
        actual = font
        if width > 32 or height > size:
            for smaller in range(size-1, 7, -1):
                actual = ImageFont.truetype(str(font_path), smaller, layout_engine=ImageFont.Layout.BASIC)
                box = actual.getbbox(ch, anchor='ls')
                width, height = box[2]-box[0], box[3]-box[1]
                if width <= 32 and height <= size:
                    break
        if width > 32 or height > size:
            raise SystemExit(f'Glyph exceeds cell: U+{cp:04X}')
        baseline = max(-box[1], min(size-4, size-box[3]))
        canvas = Image.new('L', (32, 28))
        ImageDraw.Draw(canvas).text((-box[0], baseline), ch, font=actual, anchor='ls', fill=255)
        rows = [sum(1 << (31-x) for x in range(width) if canvas.getpixel((x,y)) >= 96) for y in range(28)]
        lines.append('{0x%x,%d,%d,%d,{%s}},' % (cp, max(1, round(actual.getlength(ch))), box[0], width, ','.join(hex(r) for r in rows)))
    lines.append('};')
(ROOT/'src/native_font_data.h').write_text('\n'.join(lines)+'\n')
print(f'Generated {len(codes)} glyphs at 24px and 28px')
