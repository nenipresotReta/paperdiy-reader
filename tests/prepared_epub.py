"""Production-importer host preparation; synthetic books only."""
from hashlib import sha256
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED, ZIP_STORED

cli, importer = sys.argv[1:3]


def create(path, language="en-US", chapter=None, compression=ZIP_DEFLATED):
    if chapter is None:
        chapter = ('<html><body><h1>Example heading</h1><p>Before <em>emphasis</em> and '
                   '<strong>strong</strong>.</p><blockquote><p>Quoted sample.</p></blockquote>'
                   '<p lang="en-GB">Colour and organisation.</p><p>中文。日本語。</p></body></html>')
    with ZipFile(path, "w", compression=compression) as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        archive.writestr("META-INF/container.xml", '<container><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>')
        archive.writestr("OPS/book.opf", '<package><metadata><title>Synthetic preparation</title><language>' + language +
                         '</language></metadata><manifest><item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
                         '</manifest><spine><itemref idref="c"/></spine></package>')
        archive.writestr("OPS/chapter.xhtml", chapter)


def run(source, output, *extra, success=True):
    result = subprocess.run([cli, str(source), str(output), *extra], capture_output=True, timeout=30)
    assert result.returncode == (0 if success else 1), (result.returncode, result.stderr.decode(errors="replace"))
    if not success:
        return
    receipt = json.loads(result.stdout)
    data = output.read_bytes()
    assert data[:4] == b"PDT1" and struct.unpack_from("<III", data, 4) == (1, 160, 2)
    source_bytes, text_bytes, metadata_bytes = struct.unpack_from("<QII", data, 16)
    assert source_bytes == len(source.read_bytes())
    assert len(data) == 160 + text_bytes + metadata_bytes
    assert data[32:64] == sha256(source.read_bytes()).digest()
    assert data[64:96] == sha256(data[160:]).digest()
    assert data[100:160] == bytes(60)
    assert receipt["sourceSha256"] == data[32:64].hex()
    assert receipt["payloadSha256"] == data[64:96].hex()
    assert receipt["textBytes"] == text_bytes and receipt["metadataBytes"] == metadata_bytes
    expected = subprocess.run([importer, str(source)], capture_output=True, timeout=30)
    assert expected.returncode == 0, expected.stderr
    assert data[160:160+text_bytes] == expected.stdout
    assert source.read_bytes() == original[source]
    return receipt, data


with tempfile.TemporaryDirectory(prefix="paperdiy-prepare-test-") as temporary:
    folder = Path(temporary)
    original = {}
    for index, (language, flags, compression) in enumerate([
        ("en-US", 1, ZIP_DEFLATED), ("en-GB", 2, ZIP_STORED), ("ja", 0, ZIP_DEFLATED)
    ]):
        source = folder / f"sample-{index}.epub"
        create(source, language, compression=compression)
        original[source] = source.read_bytes()
        output = folder / (source.name + ".pdt")
        receipt, data = run(source, output)
        assert receipt["flags"] == flags and struct.unpack_from("<I", data, 96)[0] == flags
        assert receipt["metadataBytes"] > 0
        saved = output.read_bytes()
        run(source, output, success=False)
        assert output.read_bytes() == saved
        flat, _ = run(source, folder / (source.name + ".flat.pdt"), "--flat")
        assert flat["metadataBytes"] == 0
        # An existing source cannot be chosen as the output, even accidentally.
        run(source, source, success=False)
        assert source.read_bytes() == original[source]
    invalid = folder / "invalid.epub"
    invalid.write_bytes(b"invalid archive" * 8)
    invalid_out = folder / "invalid.pdt"
    run(invalid, invalid_out, success=False)
    assert not invalid_out.exists()
    missing = folder / "no-parent" / "out.pdt"
    run(source, missing, success=False)
    assert not missing.exists()
    many = folder / "many.epub"
    create(many, chapter='<html><body>' + '<p><em>Small.</em></p>' * 4200 + '</body></html>')
    original[many] = many.read_bytes()
    receipt, _ = run(many, folder / "many.pdt")
    assert receipt["metadataBytes"] == 0  # bounded collector fallback preserves complete text
    mixed = folder / "mixed.epub"
    create(mixed, language="en-GB", chapter='<html><body><p>British organisation. '
           '<span lang="fr">Une typographie extraordinaire.</span> English resumes.</p></body></html>')
    original[mixed] = mixed.read_bytes()
    rich, _ = run(mixed, folder / "mixed-rich.pdt")
    flat, _ = run(mixed, folder / "mixed-flat.pdt", "--flat")
    assert rich["metadataBytes"] > 0 and rich["flags"] == 2
    assert flat["metadataBytes"] == 0 and flat["flags"] == 0
    assert not list(folder.glob(".paperdiy-prepare-*"))
print("prepared EPUB source parity, metadata, language, bounds and nonoverwrite checks passed")
