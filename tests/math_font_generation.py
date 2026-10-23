"""Check pinned source, deterministic native raster metrics and atomic failure."""
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("math_builder", ROOT / "tools/build_math_fonts.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def main():
    source = ROOT / ".cache/STIXTwoMath-Regular.otf"
    if not source.exists():
        print("math font generation: skipped (pinned source font not cached; run tools/build_math_fonts.py)")
        return
    with tempfile.TemporaryDirectory() as td:
        a, b = Path(td) / "a.h", Path(td) / "b.h"
        builder.generate(source, a)
        builder.generate(source, b)
        assert a.read_bytes() == b.read_bytes()
        # Version metadata may differ between supported local toolchains; pixels,
        # metrics, MATH constants and provenance must match the checked-in subset.
        strip_runtime = lambda blob: b"\n".join(line for line in blob.splitlines() if not line.startswith(b"// Pillow "))
        assert strip_runtime(a.read_bytes()) == strip_runtime((ROOT / "src/math_font_data.h").read_bytes())
        bad = Path(td) / "bad.otf"
        bad.write_bytes(b"not the pinned font")
        previous = a.read_bytes()
        try:
            builder.generate(bad, a)
        except ValueError:
            pass
        else:
            raise AssertionError("Unpinned font accepted")
        assert a.read_bytes() == previous
    print("math font generation: passed")


if __name__ == "__main__":
    main()
