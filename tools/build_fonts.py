"""Build native bitmap fonts with fixed sizes, shared baselines and bearings."""
from dataclasses import dataclass
from hashlib import sha256
import os
from pathlib import Path
import tempfile
from urllib.request import urlopen

from PIL import Image, ImageDraw, ImageFont, features

from font_cmap import cmap

ROOT = Path(__file__).resolve().parents[1]
FONT_SHA = "2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca"
FONT_URL = (
    "https://raw.githubusercontent.com/notofonts/noto-cjk/"
    "9b0f1436e455d902de067a2501422e5dc71ad16b/"
    "Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Regular.otf"
)
FONT_PATH = ROOT / ".cache/NotoSerifCJKsc-Regular.otf"
SUPPLEMENT_PATH = ROOT / "tools/traditional_supplement.txt"
INK_THRESHOLD = 96


@dataclass(frozen=True)
class FontSpec:
    pixels: int
    baseline: int
    height: int
    line_advance: int


FONT_SPECS = (FontSpec(24, 26, 34, 39), FontSpec(28, 31, 39, 40))


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    advance: int
    left: int
    top: int
    width: int
    height: int
    bitmap: bytes


def atomic_write(path: Path, data: bytes) -> None:
    """A failed generation/download must leave the previous file usable."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def load_font_data(path: Path = FONT_PATH) -> bytes:
    if path.exists():
        data = path.read_bytes()
    else:
        with urlopen(FONT_URL, timeout=120) as response:
            data = response.read()
        if sha256(data).hexdigest() != FONT_SHA:
            raise ValueError("Downloaded font SHA-256 mismatch")
        atomic_write(path, data)
    if sha256(data).hexdigest() != FONT_SHA:
        raise ValueError("Font SHA-256 mismatch; remove the cached download and retry")
    return data


def repertoire() -> set[int]:
    """Fixed horizontal-reading repertoire, independent of installed fonts/books."""
    codes = set(range(32, 0x300)) | set(range(0x370, 0x530))
    codes |= set(range(0x2000, 0x2070)) | set(range(0x3000, 0x3040))
    for encoding in ("gb2312", "euc_jp"):
        for high in range(0xa1, 0xff):
            for low in range(0xa1, 0xff):
                try:
                    text = bytes((high, low)).decode(encoding)
                except UnicodeDecodeError:
                    continue
                if len(text) == 1:
                    codes.add(ord(text))
    # Fixed Traditional Chinese supplement: Big5 level 1 plus common
    # Traditional Chinese codepoints. The list is checked in, not scanned at
    # build time, so generation remains deterministic and book-independent.
    for line in SUPPLEMENT_PATH.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            codes.add(int(line, 16))
    # Vertical kana repeat marks span two ems. A horizontal line cannot render
    # them at their native dimensions; use the visible missing-glyph box.
    codes.difference_update(range(0x3031, 0x3036))
    # Two explicitly bounded Japanese name forms from the same pinned source.
    codes.update((0x9ad9, 0x20bb7))
    return codes


def rasterize(font: ImageFont.FreeTypeFont, codepoint: int) -> Glyph:
    """Preserve the original outline size and pen-relative bitmap placement."""
    character = chr(codepoint)
    left, top, right, bottom = font.getbbox(character, anchor="ls")
    canvas = Image.new("L", (right - left, bottom - top))
    ImageDraw.Draw(canvas).text(
        (-left, -top), character, font=font, anchor="ls", fill=255
    )
    monochrome = canvas.point(lambda value: 255 if value >= INK_THRESHOLD else 0)
    ink = monochrome.getbbox()
    advance = round(font.getlength(character))
    if ink is None:
        return Glyph(codepoint, advance, 0, 0, 0, 0, b"")
    # Cropping transparent edges changes bearings, never the size or baseline.
    monochrome = monochrome.crop(ink)
    width, height = monochrome.size
    stride = (width + 7) // 8
    bitmap = bytearray(stride * height)
    for row in range(height):
        for column in range(width):
            if monochrome.getpixel((column, row)):
                bitmap[row * stride + column // 8] |= 0x80 >> (column & 7)
    return Glyph(
        codepoint, advance, left + ink[0], top + ink[1],
        width, height, bytes(bitmap),
    )


def validate_glyph(glyph: Glyph, spec: FontSpec) -> None:
    if not (0 <= glyph.advance <= 255 and -128 <= glyph.left <= 127 and
            -128 <= glyph.top <= 127 and glyph.width <= 255 and glyph.height <= 255):
        raise ValueError(f"Glyph metrics overflow: U+{glyph.codepoint:04X}")
    if glyph.height and (
        spec.baseline + glyph.top < 0 or
        spec.baseline + glyph.top + glyph.height > spec.height
    ):
        raise ValueError(f"Glyph exceeds shared line box: U+{glyph.codepoint:04X}")


def render_header(font_path: Path, codes: list[int]) -> str:
    lines = [
        "// Generated from Noto Serif CJK SC (SIL OFL 1.1). Do not edit.",
        f"// Source SHA-256: {FONT_SHA}",
        "#pragma once", '#include "font_metrics.h"', "namespace reader_font {",
    ]
    for spec in FONT_SPECS:
        font = ImageFont.truetype(
            str(font_path), spec.pixels, layout_engine=ImageFont.Layout.BASIC
        )
        data = bytearray()
        lines.append(f"static const Glyph GLYPHS_{spec.pixels}[] = {{")
        for codepoint in codes:
            glyph = rasterize(font, codepoint)
            validate_glyph(glyph, spec)
            lines.append(
                f"{{0x{codepoint:x},{len(data)},{glyph.advance},"
                f"{glyph.left},{glyph.top},{glyph.width},{glyph.height}}},"
            )
            data.extend(glyph.bitmap)
        lines.append("};")
        lines.append(f"static const uint8_t BITMAP_{spec.pixels}[] = {{")
        for offset in range(0, len(data), 24):
            lines.append(",".join(f"0x{value:02x}" for value in data[offset:offset+24]) + ",")
        lines.append("};")
        size = spec.pixels
        lines.append(
            f"static const Font FONT_{size} = {{GLYPHS_{size},"
            f"sizeof(GLYPHS_{size})/sizeof(GLYPHS_{size}[0]),"
            f"BITMAP_{size},sizeof(BITMAP_{size}),"
            f"{size},{spec.baseline},{spec.height},{spec.line_advance}}};"
        )
        prefix = "LIBRARY" if size == 24 else "BODY"
        for name, value in (
            ("PIXELS", size), ("BASELINE", spec.baseline),
            ("HEIGHT", spec.height), ("LINE_ADVANCE", spec.line_advance),
        ):
            lines.append(
                f'static_assert({prefix}_{name} == {value}, "Regenerate font metrics");'
            )
    lines.append("} // namespace reader_font")
    return "\n".join(lines) + "\n"


def main() -> None:
    data = load_font_data()
    codes = sorted(repertoire() & cmap(data, 0))
    header = render_header(FONT_PATH, codes).encode("utf-8")
    atomic_write(ROOT / "src/native_font_data.h", header)
    print(
        f"Generated {len(codes)} glyphs at 24px and 28px; "
        f"FreeType {features.version('freetype2')}; "
        f"header SHA-256 {sha256(header).hexdigest()}"
    )


if __name__ == "__main__":
    main()
