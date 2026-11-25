"""Exercise staging only in owned synthetic repositories; never pass -Commit."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'Windows publication handoff')
class PublicationHandoff(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out' / 'publication-tool-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.repo = Path(tempfile.mkdtemp(dir=parent)).resolve()
        (self.repo / 'tools').mkdir()
        (self.repo / 'out').mkdir()
        (self.repo / '.superpowers').mkdir()
        (self.repo / 'dir').mkdir()
        self.env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull, GIT_CONFIG_NOSYSTEM='1')
        script = Path(os.environ.get('WF_PUBLICATION_TOOL', ROOT / 'tools/commit_publication.ps1'))
        shutil.copyfile(script, self.repo / 'tools/commit_publication.ps1')
        generator = Path(os.environ.get('WF_PUBLICATION_GENERATOR', ROOT / 'tools/prepare_publication_handoff.py'))
        shutil.copyfile(generator, self.repo / 'tools/prepare_publication_handoff.py')
        self.git('init', '-q', '--initial-branch=main')
        files = {'.gitignore': 'out/\n', 'README.md': 'fixture\n', 'public.txt': 'before\n',
                 '.mcp.json': 'private fixture\n', '.superpowers/local.txt': 'local fixture\n'}
        payload = b'commit refs/heads/main\ncommitter Fixture <fixture@example.invalid> 1 +0000\ndata 7\nfixture\n'
        for name, content in files.items():
            data = content.encode()
            payload += f'M 100644 inline {json.dumps(name)}\ndata {len(data)}\n'.encode() + data + b'\n'
        self.git('fast-import', '--quiet', data=payload + b'\ndone\n')
        self.git('read-tree', '--reset', '-u', 'HEAD')
        (self.repo / 'public.txt').write_bytes(b'after\n')
        self.head = self.git('rev-parse', 'HEAD')

    def git(self, *args, data=None):
        return subprocess.run(['git', '-C', str(self.repo), *args], input=data,
                              capture_output=True, check=True, env=self.env).stdout

    def entry(self, name):
        return dict(path=name, sha256=hashlib.sha256((self.repo / name).read_bytes()).hexdigest())

    def invoke(self, files, untrack):
        patch = self.repo / 'out/publication.patch'
        patch.write_bytes(b'fixture patch bytes are hashed, not applied\n')
        plan = dict(base=self.head.decode().strip(), patch=patch.name,
                    patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                    files=files, untrack_keep_local=untrack, message='fixture')
        manifest = self.repo / 'out/manifest.json'
        manifest.write_text(json.dumps(plan, ensure_ascii=False), encoding='utf-8-sig')
        powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        return subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                               '-File', str(self.repo / 'tools/commit_publication.ps1'),
                               '-Manifest', str(manifest)], capture_output=True, env=self.env, timeout=20)

    def reject_unchanged(self, files, untrack):
        before = (self.repo / '.git/index').read_bytes()
        result = self.invoke(files, untrack)
        self.assertNotEqual(result.returncode, 0, repr(result.stdout))
        self.assertEqual(self.git('diff', '--cached', '--name-only', '-z'), b'')
        self.assertEqual((self.repo / '.git/index').read_bytes(), before)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)

    def test_stages_unicode_and_keeps_untracked_local_bytes(self):
        name = '日本語.txt'
        (self.repo / name).write_bytes(b'new public fixture\n')
        local = {path: (self.repo / path).read_bytes() for path in ('.mcp.json', '.superpowers/local.txt')}
        result = self.invoke([self.entry('public.txt'), self.entry(name)], list(local))
        self.assertEqual(result.returncode, 0, repr(result.stderr))
        staged = self.git('-c', 'core.quotepath=false', 'diff', '--cached', '--name-only', '-z').decode().split('\0')
        self.assertEqual(set(staged) - {''}, {'public.txt', name, *local})
        for path, data in local.items():
            self.assertEqual((self.repo / path).read_bytes(), data)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)

    def test_untrack_traversal_is_rejected_before_any_staging(self):
        self.reject_unchanged([self.entry('public.txt')], ['.superpowers/../README.md'])

    def test_invalid_untrack_is_rejected_before_any_staging(self):
        self.reject_unchanged([self.entry('public.txt')], ['README.md'])

    def test_staging_cannot_hide_local_configuration_behind_dot_components(self):
        self.reject_unchanged([self.entry('dir/../.mcp.json')], [])

    def test_path_patterns_and_aliases_are_rejected(self):
        for name in ('*.txt', '/public.txt', 'dir//file.txt', 'public.txt.', 'dir\\file.txt'):
            with self.subTest(name=name):
                self.reject_unchanged([dict(path=name, sha256=None)], [])

    def test_duplicate_case_alias_is_rejected_before_any_staging(self):
        self.reject_unchanged([self.entry('public.txt'), self.entry('PUBLIC.TXT')], [])

    def test_junction_path_is_rejected(self):
        target = self.repo / 'out/private'
        target.mkdir()
        (target / 'private.txt').write_bytes(b'private fixture\n')
        link = self.repo / 'link'
        made = subprocess.run(['cmd.exe', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True)
        if made.returncode:
            self.skipTest('Native junction creation unavailable')
        self.reject_unchanged([self.entry('link/private.txt')], [])

    def prepare(self):
        return subprocess.run([sys.executable, str(self.repo / 'tools/prepare_publication_handoff.py')],
                              cwd=self.repo, capture_output=True, env=self.env, timeout=20)

    def test_handoff_preserves_unicode_paths_and_git_state(self):
        names = ['tools/日本語 tracked.txt', 'tools/日本語 new.txt']
        for name in names:
            (self.repo / name).write_bytes(b'public fixture\n')
        self.git('add', '--', names[0])
        before = (self.repo / '.git/index').read_bytes()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, repr(result.stderr))
        handoff = self.repo / 'out/publication-audit/final-handoff'
        plan = json.loads((handoff / 'manifest.json').read_text(encoding='utf-8'))
        entries = {item['path']: item for item in plan['files']}
        for name in names:
            self.assertEqual(entries[name], self.entry(name))
        self.assertEqual((self.repo / '.git/index').read_bytes(), before)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)
        self.git('apply', '--reverse', '--check', '--', str(handoff / 'publication.patch'))

    def test_handoff_refuses_native_junctions(self):
        target = self.repo / 'out/private'
        target.mkdir()
        (target / 'private.txt').write_bytes(b'private fixture\n')
        link = self.repo / 'tools/link'
        made = subprocess.run(['cmd.exe', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True)
        if made.returncode:
            self.skipTest('Native junction creation unavailable')
        before = (self.repo / '.git/index').read_bytes()
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Reparse point in handoff path', result.stderr)
        self.assertFalse((self.repo / 'out/publication-audit/final-handoff/manifest.json').exists())
        self.assertEqual((self.repo / '.git/index').read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
