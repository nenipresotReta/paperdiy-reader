"""Pinned native-JP regeneration and baked-cell geometry/provenance checks."""
from pathlib import Path
from hashlib import sha256
import re
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import build_vertical_fonts as builder

class VerticalFontTests(unittest.TestCase):
    def test_native_cells_and_exact_regeneration(self):
        self.assertEqual(sha256(builder.base.FONT_PATH.read_bytes()).hexdigest(),builder.base.FONT_SHA)
        self.assertEqual((ROOT/'src/vertical_font_data.h').read_text(),builder.render())
        self.assertEqual((ROOT/'src/vertical_orientation_data.h').read_text(),builder.orientation())
        for pixels in (12,14,24,28):
            content=(ROOT/'src/vertical_font_data.h').read_text()
            block=content.split(f'GLYPHS_{pixels}[] = {{',1)[1].split('};',1)[0]
            previous=-1
            for record in re.findall(r'\{([^}]+)\}',block):
                cp,offset,advance,left,top,width,height=[int(value,0) for value in record.split(',')]
                self.assertGreater(cp,previous);previous=cp
                self.assertGreaterEqual(left,0);self.assertGreaterEqual(top,0)
                self.assertLessEqual(left+width,pixels);self.assertLessEqual(top+height,advance)
                self.assertLessEqual(advance,pixels+2 if pixels<20 else pixels)
    def test_latin_ruby_retains_every_native_descender_pixel(self):
        from PIL import ImageFont
        content=(ROOT/'src/vertical_font_data.h').read_text()
        for pixels in (12,14):
            font=ImageFont.truetype(str(builder.base.FONT_PATH),pixels,layout_engine=ImageFont.Layout.BASIC)
            records={values[0]:values[1:] for values in
                ([int(v,0) for v in record.split(',')] for record in
                 re.findall(r'\{([^}]+)\}',content.split(f'GLYPHS_{pixels}[] = {{',1)[1].split('};',1)[0]))}
            bitmap=bytes(int(v) for v in re.findall(r'\d+',content.split(f'BITMAP_{pixels}[] = {{',1)[1].split('};',1)[0]))
            for cp in map(ord,'gjpqy'):
                offset,advance,left,top,width,height=records[cp]
                native=builder.base.rasterize(font,cp)
                self.assertEqual((width,height),(native.width,native.height))
                self.assertEqual(bitmap[offset:offset+len(native.bitmap)],native.bitmap)
                self.assertLessEqual(top+height,advance)
                self.assertLessEqual(advance,pixels+2)
                if cp==ord('j'):self.assertGreater(height,pixels)

    def test_source_regional_and_vertical_substitutions(self):
        from fontTools.ttLib import TTFont
        source=TTFont(builder.base.FONT_PATH);locl,vert=builder.substitutions(source);cmap=source.getBestCmap()
        self.assertNotEqual(cmap[ord('漢')],locl[cmap[ord('漢')]])
        for cp in map(ord,'、。「」ーぁ'):
            original=cmap[cp];regional=locl.get(original,original)
            self.assertNotEqual(original,vert.get(regional,regional))

if __name__=='__main__':unittest.main()
