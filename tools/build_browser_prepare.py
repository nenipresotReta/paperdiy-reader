#!/usr/bin/env python3
"""Build the production EPUB importer as deterministic offline browser assets.

Requires Emscripten 4.0.18 and the M5GFX revision pinned in platformio.ini.
Generated JS/WASM and receipts belong in a local build directory; only the
ASCII C++ header is checked into source. No books are inputs to this recipe.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

VERSION = '4.0.18'
EMSDK_COMMIT = 'eb5eb4b3b2b792f44ded198bfe615c985da5be2c'
M5GFX_COMMIT = '347a23274081a105cb9fc0fc3bbf0e15c78e79c3'
MINIZ_SHA256 = '8d4e50dc8eefc5ffa876a5d6ba33aa7b8f880ffd13df8b7816ef7e0c362cf41f'
MINIZ_HEADER_SHA256 = '521e3b3301bab2ed6ae266486b2a930c2aded631452d1b070c25ee3771af3f89'
ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def array(name, data):
    rows = ['  ' + ','.join(f'0x{x:02x}' for x in data[i:i+20]) + ','
            for i in range(0, len(data), 20)]
    return (f'static const uint8_t {name}[] PAPERDIY_WEB_PROGMEM = {{\n' + '\n'.join(rows) +
            f'\n}};\nstatic constexpr size_t {name[:-3]}_SIZE = sizeof({name});\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emsdk', type=Path, required=True)
    parser.add_argument('--m5gfx', type=Path, default=ROOT / '.pio/libdeps/paperdiy/M5GFX')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--header', type=Path, default=ROOT / 'src/prepared_web_assets.h')
    parser.add_argument('--max-memory-mib', type=int, default=128,
                        help='128 for production; smaller heaps are only for failure tests')
    parser.add_argument('--check', action='store_true', help='verify the existing header without rewriting it')
    args = parser.parse_args()
    if args.max_memory_mib != 128 and args.header.resolve() == ROOT/'src/prepared_web_assets.h':
        parser.error('test heap limits require a separate --header output')
    if not 16 <= args.max_memory_mib <= 128:
        parser.error('memory ceiling must be between 16 and 128 MiB')
    compiler = args.emsdk.resolve() / 'upstream/emscripten/em++'
    env = dict(os.environ, EM_CONFIG=str(args.emsdk.resolve() / '.emscripten'), SOURCE_DATE_EPOCH='0')
    version = subprocess.check_output([str(compiler), '--version'], env=env, text=True)
    if not re.search(r'\b' + re.escape(VERSION) + r'\b', version.splitlines()[0]):
        raise SystemExit('Emscripten version differs from the reviewed pin')
    m5gfx = args.m5gfx.resolve()
    source = (m5gfx / 'src/lgfx/utility/lgfx_miniz.c').read_text()
    if digest(source.encode()) != MINIZ_SHA256 or digest((m5gfx/'src/lgfx/utility/lgfx_miniz.h').read_bytes()) != MINIZ_HEADER_SHA256:
        raise SystemExit('M5GFX decompressor differs from the reviewed pin')
    marker = '\n#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 1\n'
    if source.count(marker) != 1:
        raise SystemExit('M5GFX miniz changed: review the portable configuration')
    portable = source.replace(marker, '\n#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0\n')
    build = args.build_dir.resolve();build.mkdir(parents=True, exist_ok=True)
    # A stable basename avoids path-dependent JS references and custom sections.
    miniz = build / 'lgfx_miniz_portable.c';miniz.write_text(portable)
    output = build / 'prepare-core.js'
    exports = ['_pdt_allocate', '_pdt_prepare', '_pdt_result', '_pdt_result_size', '_pdt_error', '_pdt_release']
    miniz_object = build/'lgfx_miniz_portable.o'
    subprocess.run([str(compiler.with_name('emcc')), '-Oz', '-flto', '-c', str(miniz), '-o', str(miniz_object)], env=env, check=True)
    command = [str(compiler), '-std=c++17', '-Oz', '-flto', '-fexceptions',
               '-ffile-prefix-map=' + str(ROOT) + '=.',
               '-ffile-prefix-map=' + str(build) + '=build',
               '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'lib/tinyxml2'), '-I' + str(m5gfx / 'src'),
               str(ROOT / 'tools/browser_prepare.cpp'), str(ROOT / 'lib/tinyxml2/tinyxml2.cpp'),
               str(miniz_object),
               '-sMODULARIZE=1', '-sEXPORT_NAME=PaperDIYPrepare', '-sENVIRONMENT=worker,node',
               '-sFILESYSTEM=0', '-sDYNAMIC_EXECUTION=0', '-sMALLOC=emmalloc', '-sABORTING_MALLOC=0',
               '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=16777216',
               '-sMAXIMUM_MEMORY=' + str(args.max_memory_mib * 1024 * 1024),
               '-sSTACK_SIZE=262144', '-sMEMORY_GROWTH_LINEAR_STEP=4194304',
               '-sASSERTIONS=0', '-sSUPPORT_LONGJMP=0', '-sDISABLE_EXCEPTION_CATCHING=0',
               '-sEXPORTED_FUNCTIONS=' + json.dumps(exports),
               '-sEXPORTED_RUNTIME_METHODS=' + json.dumps(['UTF8ToString', 'HEAPU8']),
               '-sINCOMING_MODULE_JS_API=' + json.dumps(['locateFile', 'wasmBinary', 'print', 'printErr', 'onAbort']),
               '-o', str(output)]
    subprocess.run(command, env=env, check=True)
    js=output.read_bytes();wasm=output.with_suffix('.wasm').read_bytes()
    # mtime=0 and a fixed OS byte make gzip identical across supported hosts.
    def compress(data):
        packed=bytearray(gzip.compress(data, compresslevel=9, mtime=0));packed[9]=255;return bytes(packed)
    js_gz=compress(js);wasm_gz=compress(wasm)
    if len(js_gz)+len(wasm_gz)>1024*1024:
        raise SystemExit('Compressed browser assets exceed the 1 MiB firmware budget')
    header = ('#pragma once\n// Generated by tools/build_browser_prepare.py; do not edit.\n'
              '// Emscripten ' + VERSION + '; source EPUBs and personal data are never embedded.\n'
              '#include <cstddef>\n#include <cstdint>\n'
              '#ifdef ARDUINO\n#include <pgmspace.h>\n#define PAPERDIY_WEB_PROGMEM PROGMEM\n'
              '#else\n#define PAPERDIY_WEB_PROGMEM\n#endif\n'
              'namespace prepared_web_assets {\n' + array('CORE_JS_GZ',js_gz) + array('CORE_WASM_GZ',wasm_gz) +
              '} // namespace prepared_web_assets\n#undef PAPERDIY_WEB_PROGMEM\n')
    if args.check:
        if not args.header.exists() or args.header.read_text() != header:
            raise SystemExit('Browser assets are stale; regenerate with the pinned toolchain')
    else:
        args.header.parent.mkdir(parents=True, exist_ok=True);args.header.write_text(header)
    receipt = {'emscripten': VERSION, 'emsdkCommit': EMSDK_COMMIT, 'm5gfxCommit': M5GFX_COMMIT,
               'maximumMemoryMiB': args.max_memory_mib,
               'jsBytes': len(js), 'wasmBytes': len(wasm), 'jsGzipBytes': len(js_gz), 'wasmGzipBytes': len(wasm_gz),
               'jsSha256': digest(js), 'wasmSha256': digest(wasm), 'headerSha256': digest(header.encode()),
               'minizSha256': digest(source.encode())}
    (build / 'browser-prepare-build.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
