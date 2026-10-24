"""Synthetic EPUB regression and resource-boundary tests; no external books."""
import json
import random
import struct
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
sidecars = 0
NONE = 2**32 - 1


def metadata(result):
    try:
        meta = json.loads(result.stderr)
    except json.JSONDecodeError as error:
        raise AssertionError(
            'Invalid EPUB structure diagnostics: ' + result.stderr.decode(errors='replace')
        ) from error
    if not meta['ready']:
        assert meta['records'] == [] and meta['resources'] == []
        return meta
    text = result.stdout
    records, resources = meta['records'], meta['resources']
    previous_resource_end = 0
    for resource in resources:
        assert previous_resource_end <= resource['begin'] <= resource['end'] <= len(text)
        text[resource['begin']:resource['end']].decode('utf-8', errors='strict')
        previous_resource_end = resource['end']
    previous_text_end = 0
    for index, record in enumerate(records):
        begin, end = record['begin'], record['end']
        assert 0 <= begin <= end <= len(text)
        text[begin:end].decode('utf-8', errors='strict')
        if record['parent'] != NONE:
            assert record['parent'] < index
            parent = records[record['parent']]
            assert parent['begin'] <= begin <= end <= parent['end']
            assert parent['resource'] == record['resource']
        if record['resource'] != NONE:
            resource = resources[record['resource']]
            assert resource['begin'] <= begin <= end <= resource['end']
        if record['kind'] in ('text', 'break'):
            assert previous_text_end <= begin
            previous_text_end = end
    # Every meaningful output byte is represented exactly once by text/title
    # spans. Generated block separators may remain uncovered by design.
    represented = b''.join(text[r['begin']:r['end']] for r in records if r['kind'] in ('text', 'break', 'title'))
    strip_space = lambda value: bytes(c for c in value if c not in b' \t\n\r')
    assert strip_space(represented) == strip_space(text), ('metadata omitted source text', represented[:100], text[:100])
    return meta


def payload(result, record):
    return result.stdout[record['begin']:record['end']].decode('utf-8')



def run(path, expected=None, error=None, mode=None):
    global cases, sidecars
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
    # Every old success/error fixture also traverses the optional observer path.
    # This is a byte-for-byte comparison, including normalization and ordering.
    if mode is None:
        observed = subprocess.run([exe, str(path), '--structure'], capture_output=True, timeout=15)
        assert observed.returncode == result.returncode, (path.name, observed.stderr)
        assert observed.stdout == result.stdout, (path.name, 'sidecar changed flat text')
        if not error:
            metadata(observed)
        else:
            assert observed.stderr == result.stderr
        sidecars += 1
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
    # Exercise the low-level inflater's bounded input/output and the separate
    # CRC check with otherwise intact ZIP directory and local-header records.
    for label, field, delta, error in [
        ('short-output', 24, -1, 'EPUB decompression failed'),
        ('long-output', 24, 1, 'EPUB decompression failed'),
        ('short-deflate', 20, -1, 'EPUB decompression failed'),
        ('wrong-crc', 16, 1, 'EPUB CRC mismatch'),
    ]:
        path = archive(label + '.epub', base)
        raw = bytearray(path.read_bytes())
        at = raw.index(b'PK\x01\x02')
        while True:
            name_size, extra_size, comment_size = struct.unpack_from('<HHH', raw, at + 28)
            if raw[at + 46:at + 46 + name_size] == b'OPS/chapter.xhtml':
                local = struct.unpack_from('<I', raw, at + 42)[0]
                value = struct.unpack_from('<I', raw, at + field)[0] + delta
                struct.pack_into('<I', raw, at + field, value)
                struct.pack_into('<I', raw, local + field - 2, value)
                break
            at += 46 + name_size + extra_size + comment_size
        path.write_bytes(raw)
        run(path, error=error)
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
    for tag, expected_language in [('en', 1), ('EN-gb', 2), ('en-US', 1), ('en-Latn-GB', 2), ('ja', 0), ('fr', 0), ('en-', 0)]:
        opf = package.replace('<package>', '<package><metadata><dc:language xmlns:dc="http://purl.org/dc/elements/1.1/">' + tag + '</dc:language></metadata>')
        result = run(archive('lang-' + tag + '.epub', {**base, 'OPS/book.opf': opf}), mode='--language')
        assert result.stderr.decode().strip() == f'HYPHEN_LANGUAGE english={int(bool(expected_language))} region={expected_language}'
        # Language upgrades of a cached book read only package metadata, even
        # when the spine entry cannot be parsed. No title/body output is emitted.
        inspected = run(archive('inspect-' + tag + '.epub', {**base, 'OPS/book.opf': opf, 'OPS/chapter.xhtml': 'deliberately invalid XHTML'}), mode='--inspect-language', expected='')
        assert inspected.stderr == result.stderr
    english_package = package.replace('<package>', '<package><metadata><language>en-GB</language></metadata>')
    for tag in ('fr', 'ja', '', 'en-US'):
        chapter = '<html><body><p>English <span lang="' + tag + '">foreign</span> text.</p></body></html>'
        fallback = run(archive('flat-language-' + (tag or 'empty') + '.epub', {**base, 'OPS/book.opf': english_package, 'OPS/chapter.xhtml': chapter}), mode='--language', expected='English foreign text.\n\n')
        assert fallback.stderr.decode().strip() == 'HYPHEN_LANGUAGE english=0 region=0'

    # Semantic records preserve actual flattened UTF-8 byte ranges. Formatting
    # and CSS never modify text, generate list labels or select a new font.
    rich_chapter = ('<html xml:lang="zh-Hans"><body><section><h2>第二章</h2>'
        '<p style="margin: 9em">First <em>em <strong>both </strong></em>last '
        '<span xml:lang="ja">日本語</span><span lang=""> unknown</span> </p>'
        '<blockquote><p>quote</p><p>next</p></blockquote>'
        '<ul><li>item<ul><li>nested</li></ul></li></ul>'
        '<pre> x\n \t<b>code </b><i> </i></pre>'
        '<p><code>end</code><br/>line<sup>2</sup><sub>3</sub></p></section></body></html>')
    rich_package = package.replace('<package>', '<package><metadata><title>Book </title><language>EN-gb</language></metadata>')
    rich = archive('structure-rich.epub', {**base, 'OPS/book.opf': rich_package, 'OPS/chapter.xhtml': rich_chapter})
    expected_rich = 'Book\n\n第二章\n\nFirst em both last 日本語 unknown\n\nquote\n\nnext\n\nitem\n\nnested\n\n x\n     code\n\nend\nline23\n\n'
    run(rich, expected=expected_rich)
    result = run(rich, expected=expected_rich, mode='--structure')
    meta = metadata(result)
    assert meta['ready'] and meta['resources'][0]['path'] == 'OPS/chapter.xhtml'
    records = meta['records']
    headings = [r for r in records if r['kind'] == 'heading']
    assert len(headings) == 1 and headings[0]['level'] == 2 and payload(result, headings[0]) == '第二章'
    assert headings[0]['language'] == 'zh-hans'
    title = next(r for r in records if r['kind'] == 'title')
    assert payload(result, title) == 'Book' and title['language'] == 'en-gb' and title['resource'] == NONE
    text_runs = [r for r in records if r['kind'] == 'text']
    assert any(payload(result, r) == 'both ' and r['flags'] == 3 for r in text_runs)
    assert any(payload(result, r) == '日本語' and r['language'] == 'ja' for r in text_runs)
    assert any(payload(result, r) == ' unknown' and r['language'] == '' for r in text_runs)
    assert any(payload(result, r) == 'code' and r['flags'] & 9 == 9 for r in text_runs)
    assert any(payload(result, r) == 'end' and r['flags'] & 4 for r in text_runs)
    assert any(payload(result, r) == '2' and r['flags'] & 16 for r in text_runs)
    assert any(payload(result, r) == '3' and r['flags'] & 32 for r in text_runs)
    quote_record = next(r for r in records if r['kind'] == 'quote')
    assert payload(result, quote_record) == 'quote\n\nnext\n\n'
    list_items = [r for r in records if r['kind'] == 'list-item']
    assert len(list_items) == 2 and payload(result, list_items[0]) == 'item\n\nnested\n\n'
    pre = next(r for r in records if r['kind'] == 'pre')
    assert payload(result, pre) == ' x\n     code' and pre['flags'] & 8
    assert len([r for r in records if r['kind'] == 'break']) == 1

    # A failed optional allocation/budget never aborts the flat text import and
    # never exposes a usable prefix that could silently hide later paragraphs.
    for mode, status in [('--structure-oom', 4), ('--structure-record-budget', 3),
                         ('--structure-path-budget', 3), ('--structure-resource-budget', 3)]:
        failed = run(rich, expected=expected_rich, mode=mode)
        failed_meta = metadata(failed)
        assert not failed_meta['ready'] and failed_meta['status'] == status

    notes_result = run(root / 'notes.epub', expected='Body 1.\n\nBody 2.\n\nNote.\n\n', mode='--structure')
    note_meta = metadata(notes_result)
    assert [r['spine'] for r in note_meta['resources']] == [0, 2, 1]
    assert [r['auxiliary'] for r in note_meta['resources']] == [0, 0, 1]
    assert [r['path'] for r in note_meta['resources']] == ['OPS/chapter.xhtml', 'OPS/chapter2.xhtml', 'OPS/notes.xhtml']
    assert [payload(notes_result, r) for r in note_meta['resources']] == ['Body 1.\n\n', 'Body 2.\n\n', 'Note.\n\n']

    # Empty and whitespace-only nested blocks can rewind a trailing text span.
    # All positions remain ordered, contained and at UTF-8 boundaries.
    for index, body in enumerate([
        '<p>A<em> </em><strong> </strong><div/><div> </div>B</p>',
        '<pre>é<b>  </b><em>  </em><div/>尾</pre>',
        '<pre> <ul><li>bar</li></ul></pre>',
        '<pre> <ol><ul><li>𠮷</li></ul></ol></pre>',
        '<div> <p> <span> </span></p><p>𠮷</p></div>',
        '<section><blockquote><div><p>x </p></div></blockquote></section>',
        '<ol><li><p>One</p><p>Two</p></li><li>Three</li></ol>',
        '<dl><dt>Term</dt><dd>Detail</dd></dl><table><tr><td>cell</td></tr></table>',
    ]):
        path = book('structure-boundary-' + str(index), '<html><body>' + body + '</body></html>')
        flat = subprocess.run([exe, str(path)], capture_output=True, check=True, timeout=15)
        observed = run(path, expected=flat.stdout.decode(), mode='--structure')
        assert metadata(observed)['ready']

    rng = random.Random(733)
    inline_tags = ('em', 'strong', 'span', 'code')
    block_tags = ('p', 'div', 'blockquote', 'ul', 'ol', 'li', 'pre')
    def random_node(depth):
        if depth == 0 or rng.randrange(3) == 0:
            return rng.choice(('Text', '漢字', '𠮷', ' ', '  ', '\n', '\t', 'é'))
        tag = rng.choice(inline_tags + block_tags)
        return '<' + tag + '>' + ''.join(random_node(depth - 1) for _ in range(rng.randrange(1, 4))) + '</' + tag + '>'
    random_content = '<html><body>' + ''.join('<section>' + random_node(5) + '</section>' for _ in range(200)) + '<p>END</p></body></html>'
    random_path = book('structure-random-nesting', random_content)
    random_flat = subprocess.run([exe, str(random_path)], capture_output=True, check=True, timeout=15)
    random_result = run(random_path, expected=random_flat.stdout.decode(), mode='--structure')
    assert metadata(random_result)['ready']
    memory_retry = run(random_path, expected=random_flat.stdout.decode(), mode='--structure-memory-retry')
    assert not metadata(memory_retry)['ready'] and metadata(memory_retry)['status'] == 5

    # Exhausted record/language budgets abandon all metadata, but the whole
    # original text, including the final sentinel, remains available.
    dense = book('structure-budget', '<html><body>' + '<p>x</p>' * 5000 + '<p>END</p></body></html>')
    dense_result = run(dense, expected=('x\n\n' * 5000 + 'END\n\n'), mode='--structure')
    assert metadata(dense_result)['status'] == 3
    languages = book('structure-language-budget', '<html><body><p>' + ''.join(f'<span lang="x-{i}">x</span>' for i in range(32)) + 'END</p></body></html>')
    language_result = run(languages, expected=('x' * 32 + 'END\n\n'), mode='--structure')
    assert metadata(language_result)['status'] == 3

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
    # Container, OPF and chapter each allocate decompressor state through the
    # importer allocator. Failing any one must be a normal bounded failure.
    for resource in range(1, 4):
        run(valid, error='PSRAM for EPUB decompressor', mode=f'--fail-inflate={resource}')
    retried = run(valid, mode='--structure-inflate-memory-retry')
    assert not metadata(retried)['ready'] and metadata(retried)['status'] == 5
    run(valid, error='PSRAM for EPUB decompressor', mode='--structure-inflate-oom')

print(f'EPUB syntax, Unicode, text fidelity, ZIP integrity, bounded semantics: {cases} cases + {sidecars} byte-identical sidecars PASS')
