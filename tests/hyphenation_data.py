#!/usr/bin/env python3
"""Preserve trie bytes and one shared C++11 definition across consumer TUs."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# SHA-256 of the pre-extraction tables: six-byte little-endian nodes, values,
# then exception UTF-8 words with NUL and little-endian uint64 break masks.
# This is independent of pointer size, source formatting and linker addresses.
TABLES = {
    'english_patterns': '9d5c2712624170c23d876ef6a76b7250c3f28fc1ad0ff7f1701ab652b2a1dcf1',
    'english_gb_patterns': 'bbb2122433ab772b04afcac44ec98c8fa3812b25ed509a3073f058744083d456',
}


def check_bytes():
    implementation = (ROOT/'src/english_hyphenation.cpp').read_text()
    for namespace, expected in TABLES.items():
        text = implementation.split('namespace ' + namespace + ' {', 1)[1].split('} // namespace', 1)[0]
        def block(name):
            return re.search(r'\b' + name + r'\[[^]]*\] = \{\n(.*?)\n\};', text, re.S)[1]
        data = bytearray()
        for child, values, count, letter in re.findall(r"\{(\d+),(\d+),(\d+),'(.)'\}", block('NODES')):
            data.extend(struct.pack('<HHBc', int(child), int(values), int(count), letter.encode()))
        data.extend(map(int, re.findall(r'\d+', block('VALUES'))))
        for word, mask in re.findall(r'\{"([^"]+)",(\d+)ULL\}', block('EXCEPTIONS')):
            data.extend(word.encode() + b'\0' + struct.pack('<Q', int(mask)))
        assert hashlib.sha256(data).hexdigest() == expected, namespace + ' table bytes changed'
    for name in ('en_us_patterns.h', 'en_gb_patterns.h'):
        header = (ROOT/'src'/name).read_text()
        assert not re.search(r'\b(?:NODES|VALUES|EXCEPTIONS)\[[^]]*\]\s*=', header), name + ' contains definitions'


COMMON = r'''
#include "english_hyphenation.h"
#include <cassert>
#include <initializer_list>
using text_language::Language;
const void* localAddress(unsigned table) {
  switch(table) {
    case 0: return english_patterns::NODES;
    case 1: return english_patterns::VALUES;
    case 2: return english_patterns::EXCEPTIONS;
    case 3: return english_gb_patterns::NODES;
    case 4: return english_gb_patterns::VALUES;
    default: return english_gb_patterns::EXCEPTIONS;
  }
}
'''
FIRST = COMMON.replace('localAddress', 'addressA') + r'''
uint64_t pointsA(const char* word, size_t size, Language language) {
  return english_hyphenation::points(word,size,language);
}
'''
SECOND = COMMON.replace('localAddress', 'addressB') + r'''
const void* addressA(unsigned);
uint64_t pointsA(const char*,size_t,Language);
int main() {
  for(unsigned i=0;i<6;++i) assert(addressA(i)==addressB(i));
  assert(pointsA("project",7,Language::EnglishUS)==0);
  assert(pointsA("project",7,Language::EnglishGB)==(1ULL<<3));
  const char* words[]={"hyphenation","international","reformation","projects","NASA","hyper-link"};
  for(const char* word:words) for(auto language:{Language::None,Language::EnglishUS,Language::EnglishGB})
    assert(pointsA(word,strlen(word),language)==english_hyphenation::points(word,strlen(word),language));
}
'''


def defined_tables(nm, path):
    listing = subprocess.check_output([nm, '-C', str(path)], text=True)
    names = [namespace+'::'+table for namespace in TABLES for table in ('NODES', 'VALUES', 'EXCEPTIONS')]
    return {name: len(re.findall(r'^.*\b[RrDdBbSsVv]\s+'+re.escape(name)+r'$', listing, re.M)) for name in names}


def check_linkage(compiler, nm):
    with tempfile.TemporaryDirectory(prefix='paperdiy-hyphenation-') as directory:
        scratch = Path(directory)
        for name, text in (('first', FIRST), ('second', SECOND)):
            source = scratch/(name+'.cpp');source.write_text(text)
            subprocess.run([compiler, '-std=c++11', '-pedantic-errors', '-O0', '-I'+str(ROOT/'src'),
                            '-c', str(source), '-o', str(scratch/(name+'.o'))], check=True)
            assert all(count == 0 for count in defined_tables(nm, scratch/(name+'.o')).values()), 'consumer owns trie bytes'
        executable = scratch/'linked'
        subprocess.run([compiler, '-std=c++11', '-pedantic-errors', '-O0', '-I'+str(ROOT/'src'),
                        str(scratch/'first.o'), str(scratch/'second.o'), str(ROOT/'src/english_hyphenation.cpp'),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        assert all(count == 1 for count in defined_tables(nm, executable).values()), 'linked image must own each table once'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=shutil.which('c++'))
    parser.add_argument('--nm', default=shutil.which('nm'))
    args = parser.parse_args()
    if not args.compiler or not args.nm:
        parser.error('C++ compiler and nm are required')
    check_bytes()
    check_linkage(args.compiler, args.nm)
    print('Unchanged US/GB trie bytes, C++11 multi-TU address identity and exactly one linked table set: PASS')
