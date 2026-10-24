"""Release boundary checks use synthetic data only."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_public_source import check_file


class PublicSourceTests(unittest.TestCase):
    def test_source_and_workflow_allowed(self):
        for name in ('src/reader.cpp', 'docs/markdown.md', '.gitattributes',
                     '.github/workflows/ci.yml', 'licenses/Noto-OFL.txt', 'README.zh-CN.md'):
            self.assertEqual(check_file(name, b'public source\n'), [])

    def test_private_and_generated_paths_rejected(self):
        for name in ('private-assets/image.h', 'books/example.md',
                     'src/shutdown-mask.h', 'backups/nvs.bin',
                     '.pio/firmware.bin', 'docs/example.epub', 'screen.png',
                     'docs/deepseek/notes.md', 'docs/handoff/notes.md'):
            self.assertTrue(check_file(name, b'synthetic'))

    def test_regular_files_only(self):
        self.assertTrue(check_file('src/link.h', b'target', '120000'))
        self.assertTrue(check_file('../src/reader.cpp', b'code'))
        self.assertTrue(check_file('/src/reader.cpp', b'code'))
        self.assertTrue(check_file('src/data.h', b'a\0b'))

    def test_credentials_not_printed_or_allowed(self):
        self.assertTrue(check_file('src/config.h', b'ghp_' + b'a' * 36))
        self.assertTrue(check_file('src/config.h', b'/' + b'Users/example/project/'))
        self.assertTrue(check_file('src/config.h', b'-----BEGIN ' + b'PRIVATE KEY-----'))
        self.assertTrue(check_file('.env', b'synthetic'))

    def test_private_build_flags_rejected(self):
        for flag in ('SHUTDOWN_HEADER', 'DIAGNOSTICS', 'DEVICE_TESTS', 'NETWORK_TESTS',
                     'EPUB_TESTS', 'USB_RAM_DISK', 'ACCEPTANCE_FIXTURE_HEADER',
                     'EXPERIMENTAL_USB_MSC'):
            self.assertTrue(check_file('platformio.ini',
                ('build_flags =\n -DPAPERDIY_' + flag + '=1\n').encode()))
        self.assertEqual(check_file('platformio.ini', b'build_flags = -DBOARD_HAS_PSRAM\n'), [])


if __name__ == '__main__':
    unittest.main()
