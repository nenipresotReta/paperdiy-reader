#!/usr/bin/env python3
"""Check a Git index or unpacked public source archive. Never print file contents.

This allowlist is a release gate, not a general secret scanner. Review outgoing
history and build firmware from the same clean, audited source separately.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess

ROOT_FILES = {'.gitattributes', '.gitignore', 'CMakeLists.txt', 'LICENSE',
              'README.md', 'README.zh-CN.md', 'THIRD_PARTY_NOTICES.md',
              'THIRD_PARTY_NOTICES.zh-CN.md', 'partitions.csv',
              'platformio.ini', 'requirements-dev.txt'}
ROOT_DIRS = {'src', 'tests', 'tools', 'lib', 'licenses', 'docs', '.github'}
EXTENSIONS = {'.h', '.hpp', '.c', '.cpp', '.py', '.js', '.md', '.txt', '.json',
              '.tex', '.yml', '.yaml', '.ini', '.csv'}
PRIVATE_DIRS = {'assets', 'private-assets', 'books', 'backups', 'snapshots',
                'handoff', 'deepseek', '.paperdiy-cache', '.pio', '.cache',
                '.venv', '__pycache__'}
RULES = [
    (re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'), 'private key'),
    (re.compile(rb'/' + rb'Users/[^/\s]+/'), 'personal absolute path'),
    (re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})\b'), 'possible token'),
]
PRIVATE_BUILD = re.compile(
    rb'-D\s*[\x27\x22]?PAPERDIY_(?:SHUTDOWN_HEADER|ACCEPTANCE_FIXTURE_HEADER|DIAGNOSTICS|DEVICE_TESTS|NETWORK_TESTS|EPUB_TESTS|USB_RAM_DISK)\b')


def check_file(name, data, mode='100644'):
    path = PurePosixPath(name)
    errors = []
    if mode not in {'100644', '100755'}:
        errors.append('non-regular entry')
    if path.is_absolute() or '..' in path.parts or not path.parts:
        errors.append('unsafe path')
    if name not in ROOT_FILES and (not path.parts or path.parts[0] not in ROOT_DIRS
                                  or path.suffix.lower() not in EXTENSIONS):
        errors.append('outside public source allowlist')
    if {part.lower() for part in path.parts} & PRIVATE_DIRS:
        errors.append('private or generated directory')
    if 'shutdown' in path.name.lower() and 'mask' in path.name.lower():
        errors.append('shutdown bitmap')
    if path.name.startswith('.env') or path.suffix.lower() in {'.pem', '.key'}:
        errors.append('credential file')
    if b'\0' in data:
        errors.append('binary source entry')
    for pattern, reason in RULES:
        if pattern.search(data):
            errors.append(reason)
    if name == 'platformio.ini' and PRIVATE_BUILD.search(data):
        errors.append('private or diagnostic build enabled')
    return errors


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args])


def index_entries(repo):
    for record in git(repo, 'ls-files', '--stage', '-z').split(b'\0'):
        if not record:
            continue
        meta, name = record.split(b'\t', 1)
        mode, oid, stage = meta.decode().split()
        if stage != '0':
            raise ValueError('unresolved Git index')
        yield name.decode('utf-8'), git(repo, 'cat-file', 'blob', oid), mode


def tree_entries(root):
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            yield name, b'', '120000'
        elif path.is_file():
            yield name, path.read_bytes(), '100644'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--index', type=Path, help='check staged blobs in a repository')
    source.add_argument('--tree', type=Path, help='check an unpacked source archive')
    args = parser.parse_args()
    root = args.index or args.tree
    if not root.is_dir():
        parser.error('source directory does not exist')
    entries = index_entries(root) if args.index else tree_entries(root)
    failures = []
    count = 0
    for name, data, mode in entries:
        count += 1
        errors = check_file(name, data, mode)
        if errors:
            failures.append({'path': name, 'reasons': errors})
    if not count:
        raise ValueError('empty source')
    print(json.dumps({'checked_files': count, 'passed': not failures,
                      'failures': failures}, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
