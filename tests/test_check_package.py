"""Rejected package members must not be opened or followed."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import check_package


class PackageInventory(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'out/package-check-tests'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tools').mkdir()
        for name in ('check_package.py', 'language_catalog.py'):
            shutil.copyfile(ROOT / 'tools' / name, self.root / 'tools' / name)
        (self.root / 'LICENSE').write_bytes(b'License\n')
        (self.root / 'LICENSES').mkdir()
        (self.root / 'LICENSES/notice.txt').write_bytes(b'Notice\n')
        (self.root / 'languages').mkdir()
        for code in ('en', 'ja'):
            (self.root / f'languages/{code}.ini').write_text(
                f'[language]\ncode={code}\nname={code}\n'
                '[messages]\nhello=Hello\n[arguments]\nhello=0\n', encoding='utf-8')
        self.payload = self.root / 'payload'
        self.payload.mkdir()
        for name in ('LICENSE', 'LICENSES', 'languages'):
            source = self.root / name
            if source.is_dir():
                shutil.copytree(source, self.payload / name)
            else:
                shutil.copyfile(source, self.payload / name)
        (self.payload / 'piero.exe').write_bytes(b'candidate executable')

    def check(self, expected_code):
        result = subprocess.run([sys.executable, str(self.root / 'tools/check_package.py'),
                                 str(self.payload)], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected_code, result.stderr)
        return json.loads(result.stdout)

    def test_complete_inventory(self):
        report = self.check(0)
        self.assertEqual(report['inventory_status'], 'PASS')
        self.assertEqual(report['release_status'], 'RELEASE BLOCKED')
        self.assertEqual(len(report['files']), 5)

    def test_unknown_file_is_rejected_without_inventorying_contents(self):
        (self.payload / 'private.bin').write_bytes(b'not package material')
        report = self.check(1)
        self.assertIn('Unreviewed package member private.bin', report['failures'])
        self.assertNotIn('private.bin', [item['path'] for item in report['files']])

    def test_sanitizer_marker_across_read_boundary(self):
        (self.payload / 'piero.exe').write_bytes(b'x' * (65536 - 4) + b'clang_rt.asan')
        report = self.check(1)
        self.assertIn('Development sanitizer dependency piero.exe', report['failures'])

    def test_unknown_notice_does_not_crash_inventory(self):
        (self.payload / 'LICENSES/unknown.txt').write_bytes(b'not a source notice')
        report = self.check(1)
        self.assertIn('Unreviewed package member LICENSES/unknown.txt', report['failures'])

    def test_reparse_file_and_directory_are_not_opened(self):
        original_lstat = Path.lstat
        original_open = Path.open
        for name in ('piero.exe', 'languages'):
            rejected = self.payload / name

            def lstat(path):
                info = original_lstat(path)
                if path == rejected:
                    return SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
                return info

            def open_file(path, *args, **kwargs):
                if path == rejected or rejected in path.parents:
                    self.fail('Opened a rejected reparse point')
                return original_open(path, *args, **kwargs)

            with self.subTest(name=name), patch.object(Path, 'lstat', lstat), \
                    patch.object(Path, 'open', open_file):
                report = check_package.inspect(self.payload, self.root)
                self.assertIn('Unexpected link ' + name, report['failures'])
                self.assertFalse(any(item['path'] == name or item['path'].startswith(name + '/')
                                     for item in report['files']))

    def test_language_limit_is_checked_while_reading(self):
        path = self.payload / 'languages/en.ini'
        path.write_bytes(b'x' * (check_package.FILE_LIMIT + 1))
        report = self.check(1)
        self.assertIn('Oversized language languages/en.ini', report['failures'])
        self.assertNotIn('languages/en.ini', [item['path'] for item in report['files']])

    def test_unknown_file_is_never_opened(self):
        rejected = self.payload / 'private.bin'
        rejected.write_bytes(b'not package material')
        original_open = Path.open

        def open_file(path, *args, **kwargs):
            if path == rejected:
                self.fail('Opened an unreviewed package member')
            return original_open(path, *args, **kwargs)

        with patch.object(Path, 'open', open_file):
            report = check_package.inspect(self.payload, self.root)
            self.assertIn('Unreviewed package member private.bin', report['failures'])

    def test_inventory_reads_have_bounded_memory(self):
        data = b'x' * 100000 + b'clang_rt.ubsan'

        class BoundedStream(io.BytesIO):
            def read(self, size=-1):
                if not 0 <= size <= 65536:
                    raise AssertionError('Unbounded package inventory read')
                return super().read(size)

        with patch.object(Path, 'open', return_value=BoundedStream(data)):
            size, sha256, sanitizer = check_package.digest(Path('fixture.exe'))
        self.assertEqual(size, len(data))
        self.assertEqual(sha256, hashlib.sha256(data).hexdigest())
        self.assertTrue(sanitizer)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows directory junction')
    def test_native_junction_is_not_traversed(self):
        import _winapi

        target = (self.root / 'outside-payload').resolve()
        target.mkdir()
        (target / 'private.bin').write_bytes(b'outside package root')
        junction = (self.payload / 'linked').absolute()
        try:
            _winapi.CreateJunction(str(target), str(junction))
        except OSError as error:
            self.skipTest(f'Cannot create fixture junction: {error}')
        self.addCleanup(junction.rmdir)
        report = self.check(1)
        self.assertIn('Unexpected link linked', report['failures'])
        self.assertFalse(any(item['path'].startswith('linked/') for item in report['files']))
        self.assertEqual((target / 'private.bin').read_bytes(), b'outside package root')


if __name__ == '__main__':
    unittest.main()
