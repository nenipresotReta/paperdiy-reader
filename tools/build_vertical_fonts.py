"""Bake bounded native Japanese vertical forms and kana ruby from pinned Noto.

Uses JAN locl then vrt2 GSUB, native vmtx/VORG origins and advances. The full
horizontal font is reused for identical glyphs. No font/runtime bitmap scaling.
"""
from pathlib import Path
from hashlib import sha256
import tempfile
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont
import build_fonts as base

ROOT = base.ROOT
ORIENTATION_SHA = "dcef09c3fb24d356b042569c328ec341efc5b53447700d799f2fb4834c3cd3cd"
ORIENTATION_URL = "https://www.unicode.org/Public/17.0.0/ucd/VerticalOrientation.txt"


def substitutions(font):
    table = font["GSUB"].table
    script = next(r.Script for r in table.ScriptList.ScriptRecord if r.ScriptTag == "hani")
    jan = next(r.LangSys for r in script.LangSysRecord if r.LangSysTag == "JAN ")
    mappings = []
    for tag in ("locl", "vrt2"):
        mapping = {}
        for index in jan.FeatureIndex:
            feature = table.FeatureList.FeatureRecord[index]
            if feature.FeatureTag == tag:
                for lookup in feature.Feature.LookupListIndex:
                    for sub in table.LookupList.Lookup[lookup].SubTable:
                        mapping.update(getattr(getattr(sub, "ExtSubTable", sub), "mapping", {}))
        mappings.append(mapping)
    return mappings


def repertoire():
    result = set(range(0x3000, 0x3100)) | set(range(32, 0x300))
    for high in range(0xa1, 0xff):
        for low in range(0xa1, 0xff):
            try:
                text = bytes((high, low)).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(text) == 1:
                result.add(ord(text))
    result.difference_update(range(0x3031, 0x3036))
    result.update((0x9ad9, 0x20bb7))
    return result


def render():
    base.load_font_data()
    source = TTFont(base.FONT_PATH)
    cmap = source.getBestCmap()
    locl, vert = substitutions(source)
    mapped = {cp: vert.get(locl.get(glyph, glyph), locl.get(glyph, glyph)) for cp, glyph in cmap.items()}
    codes = repertoire() & mapped.keys()
    override = sorted(cp for cp in codes if mapped[cp] != cmap[cp])
    # Kana and ordinary Latin ruby only; complex/nested ruby keeps flat source.
    ruby = sorted(cp for cp in codes if 32 <= cp < 0x100 or 0x3000 <= cp < 0x3100)
    for subtable in source["cmap"].tables:
        if subtable.isUnicode():
            subtable.cmap = {cp: mapped.get(cp, glyph) for cp, glyph in subtable.cmap.items()}
    del source["GSUB"]
    lines = ["// Generated native PaperDIY JP vertical/ruby subset; SIL OFL 1.1.",
             f"// Source SHA-256: {base.FONT_SHA}; JAN locl + vrt2; vmtx/VORG.",
             '#include "font_metrics.h"', 'namespace vertical_font { namespace data {']
    total = 0
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / "PaperDIYJP.otf"
        source.save(path)
        for pixels in (12, 14, 24, 28):
            font = ImageFont.truetype(str(path), pixels, layout_engine=ImageFont.Layout.BASIC)
            selected = ruby if pixels < 20 else override
            cell_height = pixels + 2 if pixels < 20 else pixels
            bitmap = bytearray()
            lines.append(f"static const reader_font::Glyph GLYPHS_{pixels}[] = {{")
            for cp in selected:
                glyph = base.rasterize(font, cp)
                name = mapped[cp]
                advance = round(source['vmtx'][name][0] * pixels / source['head'].unitsPerEm)
                origin = source['VORG'].VOriginRecords.get(name, source['VORG'].defaultVertOriginY)
                horizontal = source['hmtx'][name][0]
                left = glyph.left + round((source['head'].unitsPerEm - horizontal) * pixels / (2 * source['head'].unitsPerEm))
                top = glyph.top + round(origin * pixels / source['head'].unitsPerEm)
                if pixels < 20 and glyph.width <= pixels and glyph.height <= cell_height:
                    advance = max(advance, glyph.height)
                    # Preserve every native pixel, including Latin descenders.
                    # A small annotation cell can move the ink inside its bounds;
                    # neither the outline nor bitmap is scaled or clipped.
                    left = max(0, min(left, pixels - glyph.width))
                    top = max(0, min(top, advance - glyph.height))
                if left < 0 or top < 0 or left + glyph.width > pixels or top + glyph.height > advance or advance > cell_height:
                    # Explicitly excluded from the bounded one-em profile.
                    continue
                lines.append(f"{{0x{cp:x},{len(bitmap)},{advance},{left},{top},{glyph.width},{glyph.height}}},")
                bitmap.extend(glyph.bitmap)
            lines.append("};")
            lines.append(f"static const uint8_t BITMAP_{pixels}[] = {{")
            for offset in range(0, len(bitmap), 24):
                lines.append(",".join(str(v) for v in bitmap[offset:offset+24]) + ",")
            lines.extend(["};", f"static const reader_font::Font FONT_{pixels}={{GLYPHS_{pixels},sizeof(GLYPHS_{pixels})/sizeof(GLYPHS_{pixels}[0]),BITMAP_{pixels},sizeof(BITMAP_{pixels}),{pixels},0,{cell_height},{cell_height}}};"])
            total += len(bitmap) + len(selected) * 16
    lines.append('}} // namespace vertical_font::data')
    print(f"Native JP override/ruby flash upper estimate: {total} bytes; {len(override)} body overrides")
    return "\n".join(lines) + "\n"


def orientation():
    path = ROOT / '.cache/VerticalOrientation-17.0.0.txt'
    if not path.exists():
        from urllib.request import urlopen
        data = urlopen(ORIENTATION_URL, timeout=120).read()
        if sha256(data).hexdigest() != ORIENTATION_SHA:
            raise ValueError("Downloaded Unicode VerticalOrientation SHA-256 mismatch")
        base.atomic_write(path, data)
    data = path.read_bytes()
    if sha256(data).hexdigest() != ORIENTATION_SHA:
        raise ValueError('Unicode VerticalOrientation SHA-256 mismatch')
    ranges = []
    for line in data.decode().splitlines():
        content = line.split('#', 1)[0].strip()
        if not content:
            continue
        code, value = (s.strip() for s in content.split(';'))
        if value in ('R', 'Tr'):
            bounds = code.split('..')
            start, end = int(bounds[0], 16), int(bounds[-1], 16)
            transformed = value == 'Tr'
            if ranges and start == ranges[-1][1] + 1 and transformed == ranges[-1][2]:
                ranges[-1] = (ranges[-1][0], end, transformed)
            else:
                ranges.append((start, end, transformed))
    return '// Unicode 17.0.0 Vertical_Orientation R/Tr; generated; see licenses/Unicode.txt\n' + '\n'.join(f'{{0x{a:x},0x{b:x},{1 if transformed else 0}}},' for a, b, transformed in ranges) + '\n'


if __name__ == '__main__':
    import sys
    outputs = {ROOT/'src/vertical_font_data.h': render(), ROOT/'src/vertical_orientation_data.h': orientation()}
    for path, content in outputs.items():
        if '--check' in sys.argv:
            if path.read_text() != content:
                raise SystemExit(f'{path.name} differs; regenerate')
        else:
            base.atomic_write(path, content.encode())
