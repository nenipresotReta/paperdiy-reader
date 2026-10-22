"""Synthetic EPUB regression and resource-boundary tests; no external books."""
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
cases = 0


def run(path, expected=None, error=None, mode=None):
    global cases
    result = subprocess.run([exe, str(path)] + ([mode] if mode else []), capture_output=True, timeout=15)
    # A signal/ASan abort must never satisfy a negative test.
    assert result.returncode == (1 if error else 0), (path.name, result.returncode, result.stderr.decode(errors='replace'))
    if error:
        assert error in result.stderr.decode(), (path.name, result.stderr)
    else:
        output = result.stdout.decode('utf-8', errors='strict')
        if expected is None:
            assert 'Hello 世界 & EPUB.' in output and 'Second paragraph.' in output and 'hidden' not in output
        else:
            assert output == expected, (path.name, repr(output[:160]), repr(expected[:160]))
    cases += 1
    return result


def encryption(algorithm, resource):
    return f'<encryption><EncryptedData><EncryptionMethod Algorithm="{algorithm}"/><CipherData><CipherReference URI="{resource}"/></CipherData></EncryptedData></encryption>'


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)

    def archive(name, resources):
        path = root / name
        with ZipFile(path, 'w', ZIP_DEFLATED) as z:
            for key, value in resources.items():
                z.writestr(key, value.encode('utf-8') if isinstance(value, str) else value)
        return path

    def book(name, content):
        return archive(name + '.epub', {**base, 'OPS/chapter.xhtml': content})

    valid = archive('valid.epub', base)
    run(valid)
    directory = root / 'unpacked.epub'
    for key, value in base.items():
        p = directory / key
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value, encoding='utf-8')
    run(directory)
    run(archive('missing.epub', {k: v for k, v in base.items() if k != 'OPS/chapter.xhtml'}), error='EPUB resource missing')
    # Supply the target so the assertion tests traversal rejection, not absence.
    traversal = {**base, 'META-INF/container.xml': container.replace('OPS/book.opf', '../book.opf'), '../book.opf': package}
    run(archive('traversal.epub', traversal), error='EPUB path escapes directory')
    (root / 'book.opf').write_text(package, encoding='utf-8')
    (directory / 'META-INF/container.xml').write_text(traversal['META-INF/container.xml'], encoding='utf-8')
    run(directory, error='EPUB path escapes directory')
    (directory / 'META-INF/container.xml').write_text(container, encoding='utf-8')
    relative = {**base, 'OPS/book.opf': package.replace('chapter.xhtml', '../Text/chapter.xhtml'), 'Text/chapter.xhtml': chapter}
    run(archive('relative.epub', relative))
    long_path = package.replace('chapter.xhtml', 'a/' * 50000 + 'chapter.xhtml')
    run(archive('long-component-path.epub', {**base, 'OPS/book.opf': long_path}), error='EPUB resource path too long')
    long_root = container.replace('OPS/book.opf', 'a/' * 1000 + 'book.opf')
    run(archive('long-root-path.epub', {**base, 'META-INF/container.xml': long_root}), error='EPUB resource path too long')
    run(archive('font-obfuscation.epub', {**base, 'META-INF/encryption.xml': encryption('http://www.idpf.org/2008/embedding', 'OPS/font.otf')}))
    run(archive('encrypted-text.epub', {**base, 'META-INF/encryption.xml': encryption('http://www.idpf.org/2008/embedding', 'OPS/chapter.xhtml')}), error='Encrypted EPUB resources unsupported')
    data = archive('corrupt.epub', base)
    with ZipFile(data) as z:
        info = z.getinfo('OPS/chapter.xhtml')
        at = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
    raw = bytearray(data.read_bytes())
    raw[at + info.compress_size // 2] ^= 128
    data.write_bytes(raw)
    run(data, error='EPUB')
    data = root / 'truncated.epub'
    data.write_bytes(b'PK\x03\x04')
    run(data, error='Invalid ZIP')

    run(book('inline-spaces', '<html><body><p><span>Hello</span> <span>world</span> <em>separate</em> <b>words</b>.</p></body></html>'), expected='Hello world separate words.\n\n')
    run(book('pre-br', '<html><body><pre>line one\n  line two\n\n\tline four</pre><p>one<br/>two<br/><br/>four</p></body></html>'), expected='line one\n  line two\n\n    line four\n\none\ntwo\n\nfour\n\n')
    run(book('pre-inline-space', '<html><body><pre><span>x</span> <span>y</span>\r\n<span>z</span></pre></body></html>'), expected='x y\nz\n\n')
    run(book('entities', '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "https://example.invalid/xhtml.dtd"><html><body><p>&mdash; &copy; &ldquo;quote&rdquo; &pi; &amp;nbsp; &nbsp;</p></body></html>'), expected='— © “quote” π &nbsp; \u00a0\n\n')
    run(book('cdata-comment', '<html><body><!-- <x><x><x> &unknown; --><p><![CDATA[&mdash; <literal>]]></p></body></html>'), expected='&mdash; <literal>\n\n')
    run(book('quoted-markup', '<!DOCTYPE html SYSTEM "https://example.invalid/[><quotes]" [<!-- ] \' <x> -->]><html><body><p title="&lt;x&gt; &quot;">Text</p></body></html>'), expected='Text\n\n')
    run(book('custom-entity', '<!DOCTYPE html [<!ENTITY x "expanded">]><html><body><p>&x;</p></body></html>'), error='Custom XML entities unsupported')
    run(book('unknown-entity', '<html><body><p>&notARealEntity;</p></body></html>'), error='Unknown XML entity')
    for entity in ['&#0;', '&#xD800;', '&#x110000;', '&#99999999999999999999;', '&#x;', '&#-1;']:
        run(book('invalid-entity-' + str(cases), '<html><body><p>' + entity + '</p></body></html>'), error='XML')

    for encoding, bom in [('utf-16-le', b'\xff\xfe'), ('utf-16-be', b'\xfe\xff')]:
        resources = {k: bom + v.encode(encoding) if k.endswith(('.xml', '.opf', '.xhtml')) else v for k, v in base.items()}
        run(archive(encoding + '.epub', resources))
        run(book(encoding + '-astral', bom + '<html><body><p>𠮷😀</p></body></html>'.encode(encoding)), expected='𠮷😀\n\n')
    run(book('utf8-bom', b'\xef\xbb\xbf' + chapter.encode()))
    bad_utf8 = [b'\xff', b'\xc0\xaf', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe4\xb8', b'\x00', b'\x01']
    for i, bad in enumerate(bad_utf8):
        run(book('invalid-utf8-' + str(i), b'<html><body><p>' + bad + b'</p></body></html>'), error='XML')
    for i, bad in enumerate([b'\xff\xfe<\x00x', b'\xff\xfe\x00\xd8', b'\xfe\xff\xdc\x00']):
        run(book('invalid-utf16-' + str(i), bad), error='XML')

    notes = ('<package><manifest><item id="c" href="chapter.xhtml"/><item id="n" href="notes.xhtml"/>'
             '<item id="c2" href="chapter2.xhtml"/></manifest><spine><itemref idref="c"/>'
             '<itemref idref="n" linear="no"/><itemref idref="c2"/></spine></package>')
    run(archive('notes.epub', {**base, 'OPS/book.opf': notes, 'OPS/chapter.xhtml': '<html><body><p>Body 1.</p></body></html>', 'OPS/chapter2.xhtml': '<html><body><p>Body 2.</p></body></html>', 'OPS/notes.xhtml': '<html><body><p>Note.</p></body></html>'}), expected='Body 1.\n\nBody 2.\n\nNote.\n\n')
    for name, content in [('empty', '<html><body/></html>'), ('whitespace', '<html><head><title>Ignored</title></head><body> \n<p> </p></body></html>'), ('only-image', '<html><body><img src="cover.png"/></body></html>')]:
        run(book(name, content), error='No readable EPUB text')

    # Language metadata controls English patterns without altering extracted text.
    for tag, expected_language in [('en', 1), ('EN-gb', 1), ('ja', 0), ('fr', 0)]:
        opf = package.replace('<package>', '<package><metadata><dc:language xmlns:dc="http://purl.org/dc/elements/1.1/">' + tag + '</dc:language></metadata>')
        result = run(archive('lang-' + tag + '.epub', {**base, 'OPS/book.opf': opf}), mode='--language')
        assert result.stderr.decode().strip() == f'HYPHEN_LANGUAGE english={expected_language}'

    # A 1 MiB flat chapter remains supported; DOM density, not text length, is bounded.
    framing = '<html><body><p></p></body></html>'
    text = 'x' * (1024 * 1024 - len(framing))
    run(book('one-mib-text', '<html><body><p>' + text + '</p></body></html>'), expected=text + '\n\n')
    run(book('dense-nodes', '<html><body><p>' + '<span>x</span>' * 70000 + '</p></body></html>'), error='XML DOM resource budget exceeded')
    run(book('many-attributes', '<html><body><p ' + ' '.join(f'a{i}="v"' for i in range(70000)) + '>x</p></body></html>'), error='XML DOM resource budget exceeded')
    run(book('many-comments', '<html><body>' + '<!--x-->' * 40000 + '<p>x</p></body></html>'), error='XML DOM resource budget exceeded')
    run(book('normal-inline-density', '<html><body><p>' + '<span>x</span>' * 2000 + '</p></body></html>'), expected='x' * 2000 + '\n\n')
    run(book('depth-boundary', '<html><body>' + '<div>' * 30 + 'x' + '</div>' * 30 + '</body></html>'), expected='x\n\n')
    run(book('depth-overflow', '<html><body>' + '<div>' * 31 + 'x' + '</div>' * 31 + '</body></html>'), error='XML depth exceeds 32 levels')
    # Check all four parser entry points, including metadata before any chapter.
    deep = '<x>' * 40 + '</x>' * 40
    for resource in ['META-INF/container.xml', 'OPS/book.opf', 'META-INF/encryption.xml']:
        run(archive('deep-' + resource.replace('/', '-') + '.epub', {**base, resource: deep}), error='XML depth exceeds 32 levels')
    huge_names = {**base, **{f'{i:04d}-' + 'n' * 995: '' for i in range(140)}}
    run(archive('zip-name-budget.epub', huge_names), error='ZIP filename budget exceeded')
    run(valid, error='Not enough memory for XML document', mode='--deny-xml-memory')
    for allocation in range(1, 7):
        run(valid, error='PSRAM' if allocation > 1 else 'ZIP directory', mode=f'--fail-alloc={allocation}')

print(f'EPUB syntax, Unicode, text fidelity, ZIP integrity and resource budgets: {cases} cases PASS')
