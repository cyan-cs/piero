"""PE inventory must respect declared headers and section-backed ranges."""
import io
import struct
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from pe_imports import inventory


PE = 0x80
OPTIONAL = PE + 24
SECTION = OPTIONAL + 240
RAW = 0x200
RVA = 0x1000


def image():
    data = bytearray(0x600)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, PE)
    data[PE:PE + 4] = b'PE\0\0'
    struct.pack_into('<HH', data, PE + 4, 0x8664, 1)
    struct.pack_into('<H', data, PE + 20, 240)
    struct.pack_into('<H', data, OPTIONAL, 0x20b)
    struct.pack_into('<I', data, OPTIONAL + 60, RAW)
    struct.pack_into('<I', data, OPTIONAL + 108, 16)
    struct.pack_into('<IIII', data, SECTION + 8, 0x400, RVA, 0x400, RAW)
    struct.pack_into('<II', data, OPTIONAL + 112 + 8, RVA, 40)
    struct.pack_into('<IIIII', data, RAW, RVA + 0x80, 0, 0, RVA + 0x100, RVA + 0x90)
    data[RAW + 0x100:RAW + 0x10d] = b'KERNEL32.dll\0'
    return data


class PeInventory(unittest.TestCase):
    def inspect(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.exe'
            path.write_bytes(data)
            return inventory(path)

    def test_normal_and_delay_imports(self):
        data = image()
        struct.pack_into('<II', data, OPTIONAL + 112 + 13 * 8, RVA + 0x140, 64)
        struct.pack_into('<II', data, RAW + 0x140, 1, RVA + 0x1c0)
        data[RAW + 0x1c0:RAW + 0x1cb] = b'USER32.dll\0'
        self.assertEqual(self.inspect(data), {
            'machine': 'x64', 'imports': ['kernel32.dll', 'user32.dll'],
            'signature': 'UNSIGNED'})

    def test_absent_directories_are_not_read(self):
        data = image()
        struct.pack_into('<I', data, OPTIONAL + 108, 0)
        self.assertEqual(self.inspect(data)['imports'], [])

    def test_short_optional_header(self):
        data = image()
        struct.pack_into('<H', data, PE + 20, 2)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_directory_count_exceeds_optional_header(self):
        data = image()
        struct.pack_into('<I', data, OPTIONAL + 108, 17)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_import_table_crosses_raw_section(self):
        data = image()
        # Bytes after the section remain in the file; they are not image data.
        struct.pack_into('<I', data, SECTION + 16, 0x120)
        struct.pack_into('<II', data, OPTIONAL + 112 + 8, RVA + 0x110, 40)
        data[RAW + 0x110:RAW + 0x124] = data[RAW:RAW + 20]
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_import_name_crosses_raw_section(self):
        data = image()
        struct.pack_into('<I', data, SECTION + 16, 0x105)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_truncated_section(self):
        with self.assertRaises(ValueError):
            self.inspect(image()[:-1])

    def test_unterminated_import_descriptors(self):
        data = image()
        struct.pack_into('<I', data, OPTIONAL + 112 + 8 + 4, 20)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_invalid_certificate_range(self):
        data = image()
        struct.pack_into('<II', data, OPTIONAL + 112 + 4 * 8, len(data) - 8, 16)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_certificate_is_not_reported_as_verified(self):
        data = image()
        struct.pack_into('<II', data, OPTIONAL + 112 + 4 * 8, len(data) - 16, 16)
        self.assertEqual(self.inspect(data)['signature'], 'CERTIFICATE PRESENT, NOT VERIFIED')

    def test_header_resident_imports(self):
        data = image()
        data[0x40:0x68] = data[RAW:RAW + 40]
        struct.pack_into('<I', data, 0x40 + 12, 0x70)
        data[0x70:0x7d] = b'KERNEL32.dll\0'
        struct.pack_into('<I', data, OPTIONAL + 112 + 8, 0x40)
        self.assertEqual(self.inspect(data)['imports'], ['kernel32.dll'])

    def test_incomplete_directory(self):
        data = image()
        struct.pack_into('<I', data, OPTIONAL + 112 + 8 + 4, 0)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_legacy_delay_import_is_refused(self):
        data = image()
        struct.pack_into('<II', data, OPTIONAL + 112 + 13 * 8, RVA + 0x140, 64)
        struct.pack_into('<II', data, RAW + 0x140, 0, RVA + 0x100)
        with self.assertRaises(ValueError):
            self.inspect(data)

    def test_reads_do_not_materialize_the_executable(self):
        class BoundedStream(io.BytesIO):
            def read(self, size=-1):
                if not 0 <= size <= 256:
                    raise AssertionError('Unbounded PE inventory read')
                return super().read(size)

        with patch.object(Path, 'open', return_value=BoundedStream(image())):
            self.assertEqual(inventory('fixture.exe')['imports'], ['kernel32.dll'])


if __name__ == '__main__':
    unittest.main()
