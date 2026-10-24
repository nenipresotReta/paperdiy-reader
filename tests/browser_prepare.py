#!/usr/bin/env python3
"""Compare committed browser WASM with native production preparation.

Extracts gzip assets from the public ASCII header, reuses every regular-file
fixture from epub_fixtures.py, and generates only synthetic temporary books.
No Emscripten installation is needed to execute this regression test.
"""
import argparse
import gzip
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_STORED

ROOT = Path(__file__).resolve().parents[1]


def extract(header, name):
    match = re.search(r'\b' + name + r'\[\][^=]*=\s*\{([^}]+)\}', header)
    assert match, name
    return gzip.decompress(bytes(int(value, 16) for value in re.findall(r'0x([0-9a-f]{2})', match[1])))


def semantic_fixtures(corpus):
    """Exercise declared CSS and object codecs, not just equal flat fallbacks."""
    css = ('.vertical { writing-mode: vertical-rl; } '
           '.combine { text-combine-upright: all; } '
           '#horizontal { writing-mode: horizontal-tb; }')
    vertical = ('<p class="vertical">漢字「縦書き」、'
                '<ruby><rb>日本</rb><rp>（</rp><rt>にほん</rt><rp>）</rp></ruby>'
                '<span class="combine">12</span>年 ABC。</p>')
    horizontal = '<p id="horizontal">Horizontal control 123.</p>'
    objects = ('<p>Fraction <math xmlns="http://www.w3.org/1998/Math/MathML">'
               '<mfrac><mi>x</mi><mn>2</mn></mfrac></math> ends.</p>'
               '<math xmlns="http://www.w3.org/1998/Math/MathML" display="block">'
               '<msqrt><mrow><mi>y</mi><mo>+</mo><mn>1</mn></mrow></msqrt></math>'
               '<table><tr><th>Name</th><th>Value</th></tr>'
               '<tr><td>Alpha</td><td>12</td></tr>'
               '<tr><td>日本</td><td>東京</td></tr></table>')
    cases = {
        'semantic-external-css-vertical.epub': (vertical + horizontal, 'PDS2', True, False),
        'semantic-mathml-table.epub': (horizontal + objects, 'PDS3', False, True),
        'semantic-mixed-vertical-objects.epub': (vertical + horizontal + objects, 'PDS3', True, True),
        # Neither Japanese language/content nor RTL spine progression declares
        # vertical layout. The same ruby markup therefore stays canonical prose.
        'semantic-rtl-control.epub': ('<p>漢字<ruby>日本<rt>にほん</rt></ruby>12</p>' + horizontal,
                                      'PDS1', False, False),
    }
    for name, (body, codec, has_vertical, has_objects) in cases.items():
        linked = '<link rel="stylesheet" type="text/css" href="Styles/vertical.css"/>' if has_vertical else ''
        package = ('<package><metadata><title>Synthetic semantic parity</title><language>ja</language></metadata>'
                   '<manifest><item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
                   '<item id="css" href="Styles/vertical.css" media-type="text/css"/>'
                   '</manifest><spine page-progression-direction="rtl"><itemref idref="c"/></spine></package>')
        with ZipFile(corpus/name, 'w', compression=ZIP_STORED) as archive:
            archive.writestr('META-INF/container.xml', '<container><rootfiles><rootfile full-path="book.opf"/></rootfiles></container>')
            archive.writestr('book.opf', package)
            archive.writestr('chapter.xhtml', '<html lang="ja"><head>' + linked + '</head><body>' + body + '</body></html>')
            archive.writestr('Styles/vertical.css', css)
    return cases


def check_semantic_payload(data, spec):
    """Require meaningful PDS2/PDS3 annotations before WASM byte comparison."""
    _, codec, has_vertical, has_objects = spec
    assert data[:4] == b'PDT1' and struct.unpack_from('<I', data, 12)[0] == 2
    text_bytes, metadata_bytes = struct.unpack_from('<II', data, 24)
    text = data[160:160+text_bytes]
    metadata = data[160+text_bytes:]
    assert len(metadata) == metadata_bytes and metadata[:4] == codec.encode(), codec
    runs = struct.unpack_from('<I', metadata, 8)[0]
    header = {'PDS1':16, 'PDS2':24, 'PDS3':40}[codec]
    decoded_runs = [(struct.unpack_from('<II', metadata, header+16*i),
                     metadata[header+16*i+13]) for i in range(runs)]
    def mode_at(marker):
        at = text.index(marker.encode())
        return next(mode for (begin,end),mode in decoded_runs if begin <= at < end)
    assert mode_at('Horizontal control') == 0
    ruby_count = struct.unpack_from('<I', metadata, 16)[0] if codec != 'PDS1' else 0
    if has_vertical:
        assert mode_at('漢字') & 1, 'external stylesheet writing-mode was lost'
        assert mode_at('12年') == 3, 'inherited vertical + explicit tate-chu-yoko flags were lost'
        assert ruby_count == 1, 'ruby must be admitted rather than equally flattened'
        begin,base_end,note_begin,note_end,end = struct.unpack_from('<IIIII', metadata, header+runs*16)
        assert text[begin:base_end] == '日本'.encode()
        assert text[note_begin:note_end] == 'にほん'.encode()
        assert begin < base_end <= note_begin < note_end <= end <= len(text)
    else:
        assert not ruby_count and not any(mode for _,mode in decoded_runs)
    if has_objects:
        math_count,table_count,row_count,cell_count,tex_bytes = struct.unpack_from('<IIIII', metadata, 20)
        assert (math_count,table_count,row_count,cell_count) == (2,1,3,6)
        assert tex_bytes and b'\\frac' in metadata[-tex_bytes:] and b'\\sqrt' in metadata[-tex_bytes:]
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--importer', type=Path, required=True)
    parser.add_argument('--oom-module', type=Path)
    args = parser.parse_args()
    header = (ROOT/'src/prepared_web_assets.h').read_text()
    with tempfile.TemporaryDirectory(prefix='paperdiy-browser-test-') as temp:
        folder=Path(temp);corpus=folder/'corpus';corpus.mkdir()
        module=folder/'prepare-core.js';module.write_bytes(extract(header,'CORE_JS_GZ'))
        module.with_suffix('.wasm').write_bytes(extract(header,'CORE_WASM_GZ'))
        # Run the real existing fixture checks and preserve only distinct regular
        # archives before their temporary directory disappears. Harness-specific
        # fault injection is not copied into the browser; those archives are
        # compared using the normal native preparation contract below.
        proxy=folder/'importer_proxy.py'
        proxy.write_text('#!'+sys.executable+'\n'+
            'import hashlib,os,pathlib,sys\n'+
            'p=pathlib.Path(sys.argv[1])\n'+
            'if p.is_file():\n'+
            ' data=p.read_bytes()\n'+
            ' (pathlib.Path('+repr(str(corpus))+')/(hashlib.sha256(data).hexdigest()+".epub")).write_bytes(data)\n'+
            'os.execv('+repr(str(args.importer.resolve()))+', ['+repr(str(args.importer.resolve()))+']+sys.argv[1:])\n')
        proxy.chmod(0o700)
        subprocess.run([sys.executable,str(ROOT/'tests/epub_fixtures.py'),str(proxy)],check=True,timeout=150)
        semantic_cases = semantic_fixtures(corpus)
        # Include stored ZIP, mixed-language flat fallback, SHA padding boundaries
        # and a 64 MiB accepted source, in addition to the production corpus.
        package=('<package><metadata><title>Synthetic</title><language>en-GB</language></metadata>'
                 '<manifest><item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
                 '</manifest><spine><itemref idref="c"/></spine></package>')
        for n in [0,1,55,56,63,64,65,1000]:
            with ZipFile(corpus/f'stored-{n}.epub','w',compression=ZIP_STORED) as archive:
                archive.writestr('META-INF/container.xml','<container><rootfiles><rootfile full-path="book.opf"/></rootfiles></container>')
                archive.writestr('book.opf',package)
                archive.writestr('chapter.xhtml','<html><body><p>'+'x'*n+' 中文 日本語 <span lang="fr">bonjour</span></p></body></html>')
        # An unused stored resource makes the archive exactly 64 MiB without
        # changing canonical text, testing the inclusive source-size boundary.
        large=corpus/'size-boundary.epub'
        with ZipFile(corpus/'stored-65.epub') as archive:
            entries={name:archive.read(name) for name in archive.namelist()}
        def padded_archive(padding):
            with ZipFile(large,'w',compression=ZIP_STORED) as archive:
                for name,data in entries.items():archive.writestr(name,data)
                with archive.open('unused-padding.bin','w') as output:
                    block=bytes(1024*1024)
                    while padding:
                        count=min(len(block),padding);output.write(block[:count]);padding-=count
        padded_archive(0);padding=64*1024*1024-large.stat().st_size
        padded_archive(padding);assert large.stat().st_size==64*1024*1024
        cases=[];semantic_coverage={}
        for index,source in enumerate(sorted(corpus.glob('*.epub'))):
            for flat in [False,True]:
                output=folder/f'{index}-{int(flat)}.pdt'
                command=[str(args.native),str(source),str(output)]+(['--flat'] if flat else [])
                result=subprocess.run(command,capture_output=True,timeout=30)
                assert result.returncode in (0,1), (result.returncode,result.stderr)
                if source.name in semantic_cases:
                    assert result.returncode == 0, (source.name,result.stderr)
                    data=output.read_bytes()
                    if not flat:
                        semantic_coverage[source.name] = check_semantic_payload(data,semantic_cases[source.name])
                    else:
                        text_bytes,metadata_bytes=struct.unpack_from('<II',data,24)
                        assert metadata_bytes == 0, 'explicit flat preparation has no semantic metadata'
                        assert data[160:160+text_bytes] == semantic_coverage[source.name], 'annotations must preserve canonical UTF-8'
                cases.append({'source':str(source),'flat':flat,'expected':str(output) if result.returncode==0 else None})
        manifest={'module':str(module),'cases':cases,'workerHeader':str(ROOT/'src/prepare_worker.h')}
        if args.oom_module:manifest['oomModule']=str(args.oom_module.resolve())
        description=folder/'manifest.json';description.write_text(json.dumps(manifest))
        subprocess.run([args.node,str(ROOT/'tests/browser_prepare.js'),str(description)],check=True,timeout=180)
        print(json.dumps({'semanticByteParity':{name:spec[1] for name,spec in semantic_cases.items()},
                          'canonicalFlatParity':len(semantic_coverage)},sort_keys=True))


if __name__ == '__main__':
    main()
