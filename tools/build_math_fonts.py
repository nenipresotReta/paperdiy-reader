#!/usr/bin/env python3
"""Generate the native PaperDIY Math bitmap subset; no font file is committed.

Requires Pillow and fontTools. Source and SHA are fixed. Rasterization uses
FreeType BASIC at each native size, never scaled bitmap glyphs. MATH constants,
italic corrections and vertical variants are read from the original font.
"""
import argparse
import hashlib
import math
import os
from pathlib import Path
import tempfile
import unicodedata
from urllib.request import urlopen

import PIL
from PIL import Image, ImageDraw, ImageFont, features
import fontTools
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
FONT_URL = "https://raw.githubusercontent.com/stipub/stixfonts/v2.13b171/fonts/static_otf/STIXTwoMath-Regular.otf"
FONT_SHA256 = "3a5f3f26f40d5698b3c62dd085d48d6663696a3f80825aab8b553d5097518e8c"
PIXELS = (14, 20, 28)
INK_THRESHOLD = 96
STRETCH = tuple(map(ord, "()[]{}|")) + (0x2016, 0x221A, 0x2211, 0x220F, 0x222B, 0x222E, 0x27E8, 0x27E9)
CONSTANTS = (
    "AxisHeight", "FractionRuleThickness", "FractionNumeratorGapMin",
    "FractionNumDisplayStyleGapMin", "SuperscriptShiftUp", "SubscriptShiftDown",
    "SubSuperscriptGapMin", "SpaceAfterScript", "UpperLimitGapMin",
    "RadicalVerticalGap", "RadicalDisplayStyleVerticalGap",
)


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
            temporary = Path(f.name)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def style_maps():
    # Unicode mathematical alphabets have legacy exceptions in Letterlike Symbols.
    special = {
        ("ITALIC", "h"): 0x210E,
        **{("DOUBLE-STRUCK", ch): cp for ch, cp in zip("CHNPQRZ", (0x2102, 0x210D, 0x2115, 0x2119, 0x211A, 0x211D, 0x2124))},
        **{("SCRIPT", ch): cp for ch, cp in zip("BEFHILMR", (0x212C, 0x2130, 0x2131, 0x210B, 0x2110, 0x2112, 0x2133, 0x211B))},
        **{("SCRIPT", ch): cp for ch, cp in zip("ego", (0x212F, 0x210A, 0x2134))},
    }
    result = []
    for style, name in ((2, "ITALIC"), (3, "BOLD"), (4, "DOUBLE-STRUCK"), (5, "SCRIPT")):
        for cp in list(range(65, 91)) + list(range(97, 123)) + list(range(48, 58)) + list(range(0x391, 0x3CA)):
            ch = chr(cp)
            if ch.isascii() and ch.isalpha():
                suffix = ("CAPITAL " if ch.isupper() else "SMALL ") + ch.upper()
            elif ch.isascii() and ch.isdigit():
                suffix = "DIGIT " + unicodedata.name(ch).split()[-1]
            elif "GREEK" in unicodedata.name(ch, ""):
                suffix = unicodedata.name(ch).replace("GREEK ", "").replace(" LETTER", "")
            else:
                continue
            try:
                mapped = special.get((name, ch)) or ord(unicodedata.lookup("MATHEMATICAL " + name + " " + suffix))
            except KeyError:
                continue
            result.append((cp, style, mapped))
    for cp, mapped in ((0x3F5, 0x1D716), (0x3D1, 0x1D717), (0x3F0, 0x1D718),
                       (0x3D5, 0x1D719), (0x3F1, 0x1D71A), (0x3D6, 0x1D71B)):
        result.append((cp, 2, mapped))
    return sorted(result)


def generate(font_path, output):
    blob = font_path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != FONT_SHA256:
        raise ValueError("STIX Two Math source SHA-256 mismatch")
    source = TTFont(font_path)
    cmap = source.getBestCmap()
    math_table = source["MATH"].table
    units = source["head"].unitsPerEm
    styles = style_maps()
    symbols = "−±∓×÷⋅∘∗⋆∙∝∞∂∇√∑∏∫∮∬∭∈∉∋∅∀∃¬∧∨∩∪⊂⊃⊆⊇⊄⊊⊋⊕⊗⊥∥≤≥≠≈≃≅≡≪≫≺≻≼≽∼≍↦→←↔⇒⇐⇔↑↓↕…⋯⋮⋱ℓℏℜℑℵℶ℘℧ℝℂℕℚℤℙ°′″‴∠△□◇⌈⌉⌊⌋⟨⟩⟦⟧‖"
    wanted = set(range(32, 127)) | set(range(160, 256)) | set(range(0x391, 0x3D7))
    wanted |= set(map(ord, symbols)) | {0x3F1, 0x3F5, 0x2216} | {item[2] for item in styles}
    wanted &= set(cmap)
    variants = []
    vert = math_table.MathVariants
    for base in STRETCH:
        name = cmap.get(base)
        if name not in vert.VertGlyphCoverage.glyphs:
            continue
        construction = vert.VertGlyphConstruction[vert.VertGlyphCoverage.glyphs.index(name)]
        for record in construction.MathGlyphVariantRecord:
            if record.VariantGlyph == name:
                continue
            cp = 0xE000 + len(variants)
            variants.append((base, cp, record.VariantGlyph))
    # A temporary cmap exposes original outline variants to FreeType. No outlines
    # are transformed and this intermediate font is never distributed.
    for table in source["cmap"].tables:
        if table.isUnicode():
            for _, cp, name in variants:
                table.cmap[cp] = name
    corrections = math_table.MathGlyphInfo.MathItalicsCorrectionInfo
    italic = dict(zip(corrections.Coverage.glyphs, (v.Value for v in corrections.ItalicsCorrection)))
    glyphs, bitmap, variant_rows, constants = [], bytearray(), [], []
    with tempfile.TemporaryDirectory() as td:
        temporary_font = Path(td) / "math-variants.otf"
        source.save(temporary_font)
        full_cmap = source.getBestCmap()
        for pixels in PIXELS:
            font = ImageFont.truetype(str(temporary_font), pixels, layout_engine=ImageFont.Layout.BASIC)
            by_cp = {}
            for cp in sorted(wanted | {v[1] for v in variants}):
                ch = chr(cp)
                left, top, right, bottom = font.getbbox(ch, anchor="ls")
                w, h = max(0, right - left), max(0, bottom - top)
                raster = Image.new("L", (max(1, w), max(1, h)))
                ImageDraw.Draw(raster).text((-left, -top), ch, font=font, fill=255, anchor="ls")
                ink = raster.point(lambda value: 255 if value >= INK_THRESHOLD else 0)
                crop = ink.getbbox()
                if crop:
                    left += crop[0]
                    top += crop[1]
                    ink = ink.crop(crop)
                    w, h = ink.size
                else:
                    w = h = 0
                advance = int(math.floor(font.getlength(ch) + 0.5))
                correction = math.ceil(italic.get(full_cmap[cp], 0) * pixels / units)
                row = (cp, len(bitmap), advance, left, top, w, h, correction, pixels)
                glyphs.append(row)
                by_cp[cp] = row
                for y in range(h):
                    for x in range(0, w, 8):
                        byte = 0
                        for bit in range(8):
                            if x + bit < w and ink.getpixel((x + bit, y)):
                                byte |= 0x80 >> bit
                        bitmap.append(byte)
            for base, cp, _ in variants:
                variant_rows.append((base, cp, pixels, by_cp[cp][6]))
            values = []
            for key in CONSTANTS:
                value = getattr(math_table.MathConstants, key).Value
                values.append(max(1, math.ceil(value * pixels / units)))
            constants.append(values)
    lines = [
        "// Generated by tools/build_math_fonts.py; do not edit.",
        "// PaperDIY Math subset of STIX Two Math v2.13 b171. SIL OFL 1.1.",
        f"// Source SHA-256: {FONT_SHA256}",
        f"// Pillow {PIL.__version__}, FreeType {features.version('freetype2')}, fontTools {fontTools.__version__}; threshold {INK_THRESHOLD}.",
        "#pragma once", "namespace math_font { namespace data {",
        "static const Glyph glyphs[] = {",
    ]
    lines += ["  {" + ",".join(map(str, row)) + "}," for row in glyphs]
    lines += ["};", "static const uint8_t bitmap[] = {"]
    lines += ["  " + ",".join(f"0x{x:02x}" for x in bitmap[i:i+24]) + "," for i in range(0, len(bitmap), 24)]
    lines += ["};", "struct Variant { uint32_t base, codepoint; uint8_t pixels; int16_t height; };", "static const Variant variants[] = {"]
    lines += ["  {" + ",".join(map(str, row)) + "}," for row in sorted(variant_rows)]
    lines += ["};", "struct StyleMap { uint32_t base; uint8_t style; uint32_t codepoint; };", "static const StyleMap styles[] = {"]
    lines += ["  {" + ",".join(map(str, row)) + "}," for row in styles if row[2] in wanted]
    lines += ["};", "static const Constants constants[] = {"]
    lines += ["  {" + ",".join(map(str, row)) + "}," for row in constants]
    lines += ["};", "} }  // namespace math_font::data", ""]
    atomic_write(output, "\n".join(lines).encode())
    print(f"Generated {len(glyphs)} native glyphs, {len(bitmap)} bitmap bytes: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=ROOT / ".cache/STIXTwoMath-Regular.otf")
    parser.add_argument("--output", type=Path, default=ROOT / "src/math_font_data.h")
    args = parser.parse_args()
    if not args.font.exists():
        with urlopen(FONT_URL, timeout=30) as response:
            data = response.read(2 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != FONT_SHA256:
            raise ValueError("Downloaded font SHA-256 mismatch")
        atomic_write(args.font, data)
    generate(args.font, args.output)


if __name__ == "__main__":
    main()
