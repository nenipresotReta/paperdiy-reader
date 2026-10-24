"""Check native sizes/bearings against an independently rendered source font."""
from hashlib import sha256
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_fonts as builder


class FontGenerationTests(unittest.TestCase):
    def test_native_geometry_matches_common_baseline(self):
        self.assertEqual(sha256(builder.FONT_PATH.read_bytes()).hexdigest(), builder.FONT_SHA)
        for spec in builder.FONT_SPECS:
            font = ImageFont.truetype(
                str(builder.FONT_PATH), spec.pixels, layout_engine=ImageFont.Layout.BASIC
            )
            # Descenders, tall accents, negative bearings, punctuation and CJK.
            for character in "AgpqyjÁÀǙ（）汉字，。":
                with self.subTest(size=spec.pixels, character=character):
                    glyph = builder.rasterize(font, ord(character))
                    builder.validate_glyph(glyph, spec)
                    self.assertEqual(glyph.advance, round(font.getlength(character)))
                    # Draw the original outline on a spacious reference canvas at
                    # one shared baseline, then compare every pixel with the
                    # packed bitmap positioned using its stored bearings.
                    expected = Image.new("L", (96, 96))
                    ImageDraw.Draw(expected).text(
                        (32, 48), character, font=font, anchor="ls", fill=255
                    )
                    expected = expected.point(
                        lambda value: 255 if value >= builder.INK_THRESHOLD else 0
                    )
                    actual = Image.new("L", (96, 96))
                    stride = (glyph.width + 7) // 8
                    for row in range(glyph.height):
                        for column in range(glyph.width):
                            value = glyph.bitmap[row * stride + column // 8]
                            if value & (0x80 >> (column & 7)):
                                actual.putpixel(
                                    (32 + glyph.left + column, 48 + glyph.top + row), 255
                                )
                    self.assertEqual(actual.tobytes(), expected.tobytes())

    def test_bounded_japanese_name_forms_use_pinned_native_source(self):
        for cp in (0x9ad9,0x20bb7):
            self.assertIn(cp,builder.repertoire())
            self.assertIn(cp,builder.cmap(builder.load_font_data(),0))
            for spec in builder.FONT_SPECS:
                font=ImageFont.truetype(str(builder.FONT_PATH),spec.pixels,layout_engine=ImageFont.Layout.BASIC)
                glyph=builder.rasterize(font,cp);builder.validate_glyph(glyph,spec)
                self.assertTrue(glyph.bitmap)

    def test_vertical_two_em_symbols_are_explicitly_unsupported(self):
        self.assertTrue(set(range(0x3031, 0x3036)).isdisjoint(builder.repertoire()))

    def test_traditional_supplement_is_in_fixed_repertoire(self):
        self.assertTrue({0x8aaa, 0x5167, 0x773e, 0x2934} <= builder.repertoire())

    def test_atomic_write_failure_preserves_previous_file(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "font.h"
            target.write_bytes(b"previous valid font")
            with patch.object(builder.os, "replace", side_effect=OSError("I/O failure")):
                with self.assertRaises(OSError):
                    builder.atomic_write(target, b"incomplete new font")
            self.assertEqual(target.read_bytes(), b"previous valid font")
            self.assertEqual(list(Path(directory).iterdir()), [target])
            builder.atomic_write(target, b"complete new font")
            self.assertEqual(target.read_bytes(), b"complete new font")

    def test_invalid_cached_font_is_never_used_or_downloaded(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "font.otf"
            target.write_bytes(b"not the pinned font")
            with patch.object(builder, "urlopen") as download:
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    builder.load_font_data(target)
                download.assert_not_called()

    def test_generation_is_deterministic(self):
        codes = sorted(map(ord, "AgypjÁ汉字"))
        first = builder.render_header(builder.FONT_PATH, codes)
        second = builder.render_header(builder.FONT_PATH, codes)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
