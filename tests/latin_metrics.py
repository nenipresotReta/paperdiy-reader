"""Check deterministic design-unit metrics without rasterizing or shaping books."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_latin_metrics as builder


class LatinMetricsTests(unittest.TestCase):
    def test_checked_in_data_matches_pinned_font(self):
        self.assertEqual((ROOT / 'src/latin_metrics.h').read_text(), builder.render_header())
        self.assertEqual((ROOT / 'src/latin_metrics.cpp').read_text(), builder.render_source())

    def test_pair_adjustments_and_integer_precision(self):
        units, widths, pairs = builder.source_metrics(builder.FONT_PATH)
        count = len(builder.CODES)
        pair = lambda a, b: pairs[builder.CODES.index(ord(a)) * count + builder.CODES.index(ord(b))]
        self.assertLess(pair('A', 'V'), 0)
        self.assertLess(pair('T', 'o'), 0)
        self.assertEqual(pair(' ', ' '), 0)
        for pixels in (24, 28):
            for value in pairs:
                actual = builder.rounded(value * pixels * 16, units)
                self.assertLessEqual(abs(actual - value * pixels * 16 / units), 0.5)
                self.assertTrue(-128 <= actual <= 127)
            self.assertTrue(all(0 < builder.rounded(width * pixels * 64, units) < 65536 for width in widths))


if __name__ == '__main__':
    unittest.main()
