"""Build metadata must stay within the updater's supported SemVer subset."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_version import valid


class BuildVersion(unittest.TestCase):
    def test_supported_versions(self):
        for version in ('0.0.0', '0.0.3-dev', '1.2.3', '1.2.3-alpha.1',
                        '1.2.3-1alpha.0+build.001', '1.2.3+001', '1.2.3--',
                        '4294967295.4294967295.4294967295', '1.2.3+' + 'a' * 57):
            with self.subTest(version=version):
                self.assertTrue(valid(version))

    def test_invalid_or_unsupported_versions(self):
        for version in ('', 'v1.2.3', '1.2', '01.2.3', '1.02.3', '1.2.03',
                        '1.2.3-', '1.2.3-a..b', '1.2.3-.a', '1.2.3-a.',
                        '1.2.3-01', '1.2.3-a.01', '1.2.3+', '1.2.3+one..two',
                        '1.2.3++build', '1.2.3+one.', '4294967296.0.0',
                        '1.2.3\n', '1.2.3-\u65e5', '1.2.3+' + 'a' * 58):
            with self.subTest(version=version):
                self.assertFalse(valid(version))


if __name__ == '__main__':
    unittest.main()
