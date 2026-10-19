"""Synthetic EPUB fixtures: no personal books or external resources."""
import subprocess
import sys
import tempfile
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

exe = sys.argv[1]
container = '<container><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>'
package = ('<package><manifest><item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="f" href="font.otf" media-type="application/vnd.ms-opentype"/>'
           '</manifest><spine><itemref idref="c"/></spine></package>')
chapter = '<html><body><p>Hello 世界 &amp; EPUB.</p><script>hidden</script><p>Second paragraph.</p></body></html>'
base = {'mimetype': 'application/epub+zip', 'META-INF/container.xml': container,
        'OPS/book.opf': package, 'OPS/chapter.xhtml': chapter}

def run(path, good=True):
    result = subprocess.run([exe, str(path)], capture_output=True)
    assert (result.returncode == 0) == good, (path.name, result.returncode, result.stderr.decode(errors='replace'))
    if good:
        assert 'Hello 世界 & EPUB.' in result.stdout.decode()
        assert b'Second paragraph.' in result.stdout
        assert b'hidden' not in result.stdout
    return result

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    def archive(name, resources):
        path = root/name
        with ZipFile(path, 'w', ZIP_DEFLATED) as z:
            for key, value in resources.items():
                z.writestr(key, value.encode('utf-8'))
        return path
    run(archive('valid.epub', base))
    directory = root/'unpacked.epub'
    for key, value in base.items():
        p = directory/key
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value)
    run(directory)
    run(archive('missing.epub', {k:v for k,v in base.items() if k != 'OPS/chapter.xhtml'}), False)
    run(archive('traversal.epub', {**base, 'META-INF/container.xml': container.replace('OPS/book.opf', '../book.opf')}), False)
    def encryption(algorithm, resource):
        return f'<encryption><EncryptedData><EncryptionMethod Algorithm="{algorithm}"/><CipherData><CipherReference URI="{resource}"/></CipherData></EncryptedData></encryption>'
    run(archive('font-obfuscation.epub', {**base, 'META-INF/encryption.xml': encryption('http://www.idpf.org/2008/embedding', 'OPS/font.otf')}))
    run(archive('encrypted-text.epub', {**base, 'META-INF/encryption.xml': encryption('http://www.idpf.org/2008/embedding', 'OPS/chapter.xhtml')}), False)
    data = archive('corrupt.epub', base)
    with ZipFile(data) as z:
        info = z.getinfo('OPS/chapter.xhtml')
        at = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
    raw = bytearray(data.read_bytes()); raw[at + info.compress_size//2] ^= 128; data.write_bytes(raw)
    run(data, False)
    data = root/'truncated.epub'; data.write_bytes(b'PK\x03\x04')
    run(data, False)
print('EPUB compressed/directory, Unicode, missing resources, traversal, encryption, corruption: PASS')
