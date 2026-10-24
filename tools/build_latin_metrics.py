"""Deterministic fixed-point Latin advances/GPOS pairs from the pinned open font.

The device keeps native glyph bitmaps. Advances use 1/64 px and pair adjustments
use 1/16 px; dense signed-byte pair tables trade 21 KiB of flash for O(1) lookup.
No shaping library, font parser or heap allocation runs on the reader.
"""
import argparse
from pathlib import Path
from fontTools.ttLib import TTFont
from build_fonts import FONT_PATH, FONT_SHA, atomic_write, load_font_data

ROOT = Path(__file__).resolve().parents[1]
CODES = tuple(range(32, 127)) + (0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026)


def rounded(value: int, divisor: int) -> int:
    return (value + divisor // 2) // divisor if value >= 0 else -((-value + divisor // 2) // divisor)


def source_metrics(path: Path):
    load_font_data(path)  # Reject a substituted or corrupt font before parsing it.
    with TTFont(path) as font:
        units = font['head'].unitsPerEm
        cmap = font.getBestCmap()
        gpos = font['GPOS'].table
        indices = sorted({index for record in gpos.FeatureList.FeatureRecord
                          if record.FeatureTag == 'kern'
                          for index in record.Feature.LookupListIndex})
        lookups = [gpos.LookupList.Lookup[index] for index in indices]
        for lookup in lookups:
            if lookup.LookupType != 2:
                raise ValueError('Review changed GPOS kerning lookup')
            for table in lookup.SubTable:
                if table.Format not in (1, 2) or table.ValueFormat1 != 4 or table.ValueFormat2 != 0:
                    raise ValueError('Review changed GPOS pair positioning')
        advances = [font['hmtx'][cmap[code]][0] for code in CODES]

        def pair(first, second):
            adjustment = 0
            for lookup in lookups:
                for table in lookup.SubTable:
                    if first not in table.Coverage.glyphs:
                        continue
                    if table.Format == 1:
                        records = table.PairSet[table.Coverage.glyphs.index(first)].PairValueRecord
                        match = next((item for item in records if item.SecondGlyph == second), None)
                        if match is None:
                            continue
                        adjustment += match.Value1.XAdvance
                    else:
                        left = table.ClassDef1.classDefs.get(first, 0)
                        right = table.ClassDef2.classDefs.get(second, 0)
                        adjustment += table.Class1Record[left].Class2Record[right].Value1.XAdvance
                    break  # The first applicable subtable satisfies this lookup.
            return adjustment

        pairs = [pair(cmap[first], cmap[second]) for first in CODES for second in CODES]
        return units, advances, pairs


def render_header() -> str:
    lines = ['// Generated from Noto Serif CJK SC (SIL OFL 1.1). Do not edit.',
             f'// Source SHA-256: {FONT_SHA}', '#pragma once', '#include <cstdint>',
             'namespace latin_metrics {', f'constexpr int COUNT = {len(CODES)};']
    for size in (24, 28):
        lines.append(f'extern const uint16_t ADVANCE_{size}[COUNT];')
        lines.append(f'extern const int8_t KERN_{size}[COUNT * COUNT];')
    lines += ['inline int index(uint32_t cp) {', '  if(cp>=32&&cp<=126)return int(cp)-32;', '  switch(cp){']
    lines += [f'    case 0x{cp:x}:return {index};' for index, cp in enumerate(CODES) if cp > 126]
    lines += ['    default:return -1;', '  }', '}',
              'inline int advance64(unsigned pixels,uint32_t cp,int fallback) {',
              '  const int at=index(cp);',
              '  return at<0?fallback*64:(pixels==24?ADVANCE_24[at]:ADVANCE_28[at]);', '}',
              'inline int kern64(unsigned pixels,uint32_t before,uint32_t after) {',
              '  const int a=index(before),b=index(after);if(a<0||b<0)return 0;',
              '  return int(pixels==24?KERN_24[a*COUNT+b]:KERN_28[a*COUNT+b])*4;', '}',
              '} // namespace latin_metrics', '']
    return '\n'.join(lines)


def render_source(path: Path = FONT_PATH) -> str:
    units, advances, pairs = source_metrics(path)
    lines = ['// Generated from Noto Serif CJK SC (SIL OFL 1.1). Do not edit.',
             f'// Source SHA-256: {FONT_SHA}', '#include "latin_metrics.h"',
             'namespace latin_metrics {']
    for size in (24, 28):
        widths = [rounded(value * size * 64, units) for value in advances]
        kern = [rounded(value * size * 16, units) for value in pairs]
        if not all(-128 <= value <= 127 for value in kern):
            raise ValueError('Kerning exceeds signed byte range')
        lines.append(f'const uint16_t ADVANCE_{size}[COUNT] = {{')
        lines.extend(','.join(map(str, widths[at:at + 16])) + ',' for at in range(0, len(widths), 16))
        lines.append('};')
        lines.append(f'const int8_t KERN_{size}[COUNT * COUNT] = {{')
        lines.extend(','.join(map(str, kern[at:at + len(CODES)])) + ',' for at in range(0, len(kern), len(CODES)))
        lines.append('};')
    lines += ['} // namespace latin_metrics', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, default=FONT_PATH)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = {'latin_metrics.h': render_header(), 'latin_metrics.cpp': render_source(args.font)}
    for name, content in outputs.items():
        target = ROOT / 'src' / name
        rendered = content.encode()
        if args.check:
            if target.read_bytes() != rendered:
                raise SystemExit(f'{name} differs; regenerate with the pinned font')
        else:
            atomic_write(target, rendered)
