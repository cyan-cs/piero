"""A failed trust-header installation must fail the CMake generation command."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SyntaxHeaders(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/syntax-codegen-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.cmake = shutil.which('cmake')
        self.assertIsNotNone(self.cmake)
        self.dll = self.root / 'host.dll'
        self.dll.write_bytes(b'fixture image bytes')
        for base in ('bin', 'pack'):
            (self.root / base / 'grammars/json' if base == 'bin' else
             self.root / base / 'json').mkdir(parents=True)
        (self.root / 'bin/grammars/json/parser.dll').write_bytes(b'fixture parser')
        for name in ('highlights.scm', 'manifest.toml'):
            (self.root / 'pack/json' / name).write_bytes(b'fixture metadata')

    def generate(self, kind, output):
        return subprocess.run([self.cmake, f'-DOUT={output}', f'-DDLL={self.dll}',
                               f'-DBIN={self.root / "bin"}', f'-DPACK={self.root / "pack"}',
                               '-DIDS=json', '-P', str(ROOT / f'cmake/gen_syntax_{kind}.cmake')],
                              capture_output=True)

    def test_generated_headers(self):
        for kind, marker in (('pin', b'WF_SYNTAX_DLL_SHA256'), ('trust', b'WF_SYN_TRUST_COUNT 1u')):
            with self.subTest(kind=kind):
                output = self.root / f'{kind}.h'
                result = self.generate(kind, output)
                self.assertEqual(result.returncode, 0, repr(result.stderr))
                self.assertIn(marker, output.read_bytes())
                self.assertFalse(Path(str(output) + '.tmp').exists())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows file sharing refusal')
    def test_locked_header_fails_without_replacing_it(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        create = kernel.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                           wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = kernel.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close.restype = wintypes.BOOL
        for kind in ('pin', 'trust'):
            with self.subTest(kind=kind):
                output = self.root / f'{kind}.h'
                output.write_bytes(b'preserve old header')
                handle = create(str(output), 0x80000000, 1, None, 3, 0, None)
                self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
                try:
                    result = self.generate(kind, output)
                finally:
                    self.assertTrue(close(handle))
                self.assertEqual(output.read_bytes(), b'preserve old header')
                self.assertNotEqual(result.returncode, 0, 'Header copy failed but generation succeeded')


if __name__ == '__main__':
    unittest.main()
