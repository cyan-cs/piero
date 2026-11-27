"""Missing embedded/inline vendor notices must fail the distribution check."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import audit_licenses as audit


class NoticeCoverage(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'out/license-audit-tests'
        base.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.vendor = self.root / audit.VENDOR / 'fixture'
        self.vendor.mkdir(parents=True)
        (self.root / 'LICENSES').mkdir()
        self.notice = self.root / 'LICENSES/fixture.txt'
        (self.vendor / 'LICENSE').write_text('Base permission notice', encoding='utf-8')
        (self.vendor / 'nested').mkdir()
        (self.vendor / 'nested/LICENSE').write_text('Nested ICU permission notice', encoding='utf-8')
        self.canonical = 'Base permission notice\nNested ICU permission notice\n'
        self.notice.write_text(self.canonical, encoding='utf-8')
        self.source = self.vendor / 'code.c'
        self.source.write_text('/* Copyright Holder A. Redistribution and use allowed. */', encoding='utf-8')
        for mapping, values in (
                (audit.BUNDLES, {'fixture': 'fixture.txt'}),
                (audit.BASE_LICENSES, {'fixture.txt': ['fixture/LICENSE', 'fixture/nested/LICENSE']})):
            patched = patch.dict(mapping, values, clear=True)
            patched.start()
            self.addCleanup(patched.stop)

    def test_individual_copyright_is_required_in_addition_to_aggregate(self):
        self.assertTrue(audit.check_notices([self.source], self.root))
        self.notice.write_text(self.canonical + self.source.read_text(encoding='utf-8'), encoding='utf-8')
        self.assertEqual([], audit.check_notices([self.source], self.root))

    def test_nested_license_is_required_even_when_main_license_is_present(self):
        self.notice.write_text('Base permission notice\n' + self.source.read_text(encoding='utf-8'), encoding='utf-8')
        errors = audit.check_notices([self.source], self.root)
        self.assertEqual(1, len(errors))
        self.assertIn('fixture/nested/LICENSE', errors[0])

    def test_embedded_notice_late_in_a_header_is_checked(self):
        first = self.source.read_text(encoding='utf-8')
        second = '/* Copyright Holder B. Binary redistributions must retain this notice. */'
        header = self.vendor / 'inline.h'
        header.write_text(first + '\n' + ' ' * 50000 + second, encoding='utf-8')
        self.notice.write_text(self.canonical + first, encoding='utf-8')
        self.assertTrue(any('notice 2' in e for e in audit.check_notices([header], self.root)))
        self.notice.write_text(self.canonical + first + '\n' + second, encoding='utf-8')
        self.assertEqual([], audit.check_notices([header], self.root))

    def test_new_component_is_not_silently_covered_by_another_library(self):
        unknown = self.root / audit.VENDOR / 'unreviewed/new.c'
        unknown.parent.mkdir()
        unknown.write_text('int new_library;', encoding='utf-8')
        self.assertTrue(any('Unreviewed vendor component' in e
                            for e in audit.check_notices([unknown], self.root)))

    def test_spdx_and_icu_line_comments_are_not_skipped(self):
        text = '// SPDX-License-Identifier: 0BSD\n\nint x;\n'
        text += '// Copyright Unicode\n// License & terms of use: upstream\n'
        self.source.write_text(text, encoding='utf-8')
        errors = audit.check_notices([self.source], self.root)
        self.assertEqual(2, len(errors))
        self.notice.write_text(self.canonical + text, encoding='utf-8')
        self.assertEqual([], audit.check_notices([self.source], self.root))

    def test_new_grammar_requires_its_own_license_review(self):
        source = self.root / audit.VENDOR / 'ts-grammars/new-language/src/parser.c'
        source.parent.mkdir(parents=True)
        source.write_text('/* Generated parser; no copyright in this file. */', encoding='utf-8')
        with patch.dict(audit.BUNDLES, {'ts-grammars': 'fixture.txt'}):
            self.assertTrue(any('Unreviewed grammar' in e
                                for e in audit.check_notices([source], self.root)))

    def test_only_the_reviewed_shared_xml_header_is_allowed(self):
        header = self.root / audit.VENDOR / 'ts-grammars/common/scanner.h'
        header.parent.mkdir(parents=True)
        header.write_text('/* shared XML scanner */', encoding='utf-8')
        unknown = header.with_name('new.h')
        unknown.write_text('/* new shared code */', encoding='utf-8')
        with patch.dict(audit.BUNDLES, {'ts-grammars': 'fixture.txt'}):
            self.assertEqual([], audit.check_notices([header], self.root))
            self.assertTrue(any('Unreviewed grammar' in e
                                for e in audit.check_notices([unknown], self.root)))

    def test_data_and_source_helper_notice_removal_is_detected(self):
        errors = audit.check_additional_notices(self.root)
        self.assertTrue(any('unicode-data.txt' in e for e in errors))
        self.assertTrue(any('COPYING.GPLv2' in e for e in errors))
        self.assertTrue(any('COPYING.GPLv3' in e for e in errors))
        self.assertTrue(any('archive_string_composition.h' in e for e in errors))

    def test_legacy_unicode_table_cannot_replace_the_reviewed_update(self):
        header = self.root / 'src/third_party/libarchive-3.8.10/libarchive/archive_string_composition.h'
        header.parent.mkdir(parents=True)
        header.write_text('/* Generated from UnicodeData 6.0.0; libarchive copyright */', encoding='utf-8')
        self.assertTrue(any('Incomplete additional' in e and 'archive_string_composition.h' in e
                            for e in audit.check_additional_notices(self.root)))
        header.write_text('http://unicode.org/Public/18.0.0/ucd/UnicodeData.txt\n'
                          'UNICODE LICENSE V3\nCopyright © 1991-2026 Unicode, Inc.', encoding='utf-8')
        self.assertFalse(any('archive_string_composition.h' in e
                             for e in audit.check_additional_notices(self.root)))


if __name__ == '__main__':
    unittest.main()
