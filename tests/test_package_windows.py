"""Exercise candidate packaging with real PE parsing and ZIP verification."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_windows
from test_pe_imports import image, RAW


class CandidatePackage(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'out' / 'package-tests'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / 'build'
        self.build.mkdir()
        (self.root / 'out').mkdir()
        (self.root / 'LICENSE').write_bytes(b'Project license\n')
        (self.root / 'LICENSES').mkdir()
        (self.root / 'LICENSES/notice.txt').write_bytes(b'Dependency notice\n')
        (self.root / 'languages').mkdir()
        for code in ('en', 'ja'):
            (self.root / f'languages/{code}.ini').write_text(
                f'[language]\ncode={code}\nname={code}\n'
                '[messages]\nhello=Hello\n[arguments]\nhello=0\n', encoding='utf-8')
        (self.build / 'CMakeCache.txt').write_text(
            'CMAKE_BUILD_TYPE:STRING=Release\nWF_SANITIZE:BOOL=OFF\n'
            'CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded\n'
            'PIERO_VERSION:STRING=1.2.3\n', encoding='utf-8')
        self.output = self.root / 'out/candidate'
        self.addCleanup(patch.stopall)
        patch.object(package_windows, 'ROOT', self.root).start()
        # The synthetic PE tests the inventory, but cannot execute --version.
        self.run_version = patch.object(package_windows.subprocess, 'run',
            return_value=SimpleNamespace(stdout=b'piero 1.2.3\n')).start()

    def executables(self, dependency):
        data = image()
        struct.pack_into('<13s', data, RAW + 0x100, dependency)
        for name in ('piero.exe', 'piero_path.exe'):
            (self.build / name).write_bytes(data)

    def prepare(self):
        with contextlib.redirect_stdout(io.StringIO()):
            package_windows.prepare(self.build, self.output)

    def test_restart_manager_system_dependency(self):
        self.executables(b'RstrtMgr.dll\0')
        self.prepare()
        report = json.loads((self.output / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(report['pe']['piero.exe']['imports'], ['rstrtmgr.dll'])
        self.assertEqual(report['release_status'], 'RELEASE BLOCKED')
        self.run_version.assert_called_once_with(
            [str(self.build / 'piero.exe'), '--version'], capture_output=True,
            check=True, timeout=15)
        with zipfile.ZipFile(self.output / 'piero-windows-x64.zip') as archive:
            self.assertEqual(set(archive.namelist()), {
                'piero.exe', 'piero-path.exe', 'LICENSE', 'LICENSES/notice.txt',
                'languages/en.ini', 'languages/ja.ini'})
            self.assertEqual(archive.read('piero.exe'), (self.build / 'piero.exe').read_bytes())

    def test_unreviewed_dependency_is_refused(self):
        self.executables(b'custom.dll\0')
        with self.assertRaisesRegex(ValueError, 'Unreviewed/non-system DLL'):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.run_version.assert_not_called()

    def test_invalid_build_version_is_refused_before_execution(self):
        self.executables(b'KERNEL32.dll\0')
        invalid = '1.2.3-a..b'
        cache = self.build / 'CMakeCache.txt'
        cache.write_text(cache.read_text(encoding='utf-8').replace('1.2.3', invalid),
                         encoding='utf-8')
        self.run_version.return_value.stdout = ('piero ' + invalid + '\n').encode()
        with self.assertRaisesRegex(ValueError, 'version'):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.run_version.assert_not_called()


if __name__ == '__main__':
    unittest.main()
