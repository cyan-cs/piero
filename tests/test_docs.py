"""Regression checks for references that used to escape documentation validation."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DocumentationChecks(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'out' / 'docs-check-tests'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temp.name)
        self.assertTrue(self.root.resolve().is_relative_to(base.resolve()))
        self.addCleanup(self.temp.cleanup)
        (self.root / 'tools').mkdir()
        for lang in ('en', 'ja'):
            (self.root / 'docs' / lang).mkdir(parents=True)
            self.write(f'docs/{lang}/guide.md', '# Guide\n\n## Section\n')
        for name in ('README.md', 'CONTRIBUTING.md', 'SECURITY.md', 'CODING_STYLE.md'):
            self.write(name, '# Guide\n')
        for script in ('sync_doc_index.py', 'sync_readme.py'):
            self.write('tools/' + script, 'raise SystemExit(0)\n')
        shutil.copyfile(ROOT / 'tools/check_docs.py', self.root / 'tools/check_docs.py')
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True,
                       capture_output=True)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')

    def check(self, expected=None):
        env = dict(os.environ, PYTHONIOENCODING='utf-8')
        # Invocation outside the fixture also tests that Git uses the script's root.
        result = subprocess.run([sys.executable, str(self.root / 'tools/check_docs.py')],
                                cwd=ROOT, capture_output=True, text=True,
                                encoding='utf-8', env=env)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0 if expected is None else 1, output)
        if expected:
            self.assertIn(expected, output)

    def test_cross_file_and_duplicate_heading_anchors(self):
        self.write('docs/README.md', '# Start\n\n[Go](en/guide.md#section-1)\n')
        for lang in ('en', 'ja'):
            self.write(f'docs/{lang}/guide.md', '# Guide\n\n## Section\n\n## Section\n')
        self.check()

    def test_same_file_missing_anchor(self):
        self.write('README.md', '# Guide\n\n[Go](#removed)\n')
        self.check('missing heading #removed')

    def test_unicode_heading_anchor(self):
        self.write('docs/README.md', '# Start\n\n[Go](#%E8%A8%AD%E5%AE%9A)\n\n## 設定\n')
        self.check()

    def test_missing_source_reference(self):
        self.write('docs/README.md', '# Start\n\nSee `src/removed.c`.\n')
        self.check('missing/mis-cased source reference src/removed.c')

    def test_source_reference_case(self):
        self.write('src/real.c', '/* source */\n')
        self.write('docs/README.md', '# Start\n\nSee `src/Real.c`.\n')
        self.check('missing/mis-cased source reference src/Real.c')

    def test_ignored_link_target(self):
        self.write('.gitignore', 'generated.txt\n')
        self.write('generated.txt', 'not shipped\n')
        self.write('README.md', '# Guide\n\n[Go](generated.txt)\n')
        self.check('linked file is ignored generated.txt')

    def test_root_document_formatting(self):
        self.write('docs/README.md', '# Start\n\nTrailing space. \n')
        self.check('trailing whitespace')

    def test_readme_synchronization_is_required(self):
        self.write('tools/sync_readme.py', 'raise SystemExit("README differs")\n')
        self.check('README differs')


if __name__ == '__main__':
    unittest.main()
