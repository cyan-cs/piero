"""The review manifest preserves native Unicode paths without changing Git state."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'Windows PowerShell baseline tool')
class BaselineManifest(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'out/baseline-tests'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith('GIT_')}
        self.environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                                GIT_CONFIG_NOSYSTEM='1')
        (self.root / 'tools').mkdir()
        (self.root / 'out').mkdir()
        (self.root / '.gitignore').write_text('out/\n', encoding='utf-8')
        for name in ('prepare_baseline.ps1', 'wf_hash.ps1', 'wf_git.ps1'):
            shutil.copyfile(ROOT / 'tools' / name, self.root / 'tools' / name)
        for name in ('CMakeLists.txt', '.gitattributes', 'README.md'):
            (self.root / name).write_bytes(b'fixture\n')
        self.git('init', '-q')
        self.git('add', '.')
        # A history is needed only inside this disposable fixture repository.
        hooks = self.root / 'out/empty-hooks'
        hooks.mkdir()
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 '-c', f'core.hooksPath={hooks}', 'commit', '--no-gpg-sign', '-qm', 'fixture')

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.root), *args], check=True,
                              capture_output=True, env=self.environment).stdout

    def prepare(self, output, cwd=None):
        return subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                               '-File', str(self.root / 'tools/prepare_baseline.ps1'),
                               '-Output', str(output)], cwd=cwd, capture_output=True,
                              env=self.environment)

    def test_unicode_tracked_and_untracked_paths(self):
        names = ['日本語 空白.txt', 'café.txt']
        for name in names:
            (self.root / name).write_bytes(b'review this file\n')
        self.git('add', '--', names[0])
        before = self.git('status', '--porcelain=v1', '-z')
        output = self.root / 'out/manifest.json'
        result = self.prepare(output)
        self.assertEqual(result.returncode, 0, repr(result.stderr))
        report = json.loads(output.read_text(encoding='utf-8-sig'))
        files = {item['path']: item for item in report['files']}
        for name in names:
            self.assertEqual(files[name]['sha256'].lower(),
                             hashlib.sha256((self.root / name).read_bytes()).hexdigest())
        self.assertEqual(report['git_head'], self.git('rev-parse', 'HEAD').decode().strip())
        self.assertEqual(report['result'], 'READY_FOR_REVIEW')
        self.assertEqual(self.git('status', '--porcelain=v1', '-z'), before)

    def test_existing_manifest_is_preserved(self):
        output = self.root / 'out/manifest.json'
        output.write_bytes(b'preserve existing review')
        self.assertNotEqual(self.prepare(output).returncode, 0)
        self.assertEqual(output.read_bytes(), b'preserve existing review')

    def test_relative_output_uses_callers_directory(self):
        existing = self.root / 'manifest.json'
        existing.write_bytes(b'preserve repository manifest')
        caller = self.root / 'out/caller'
        caller.mkdir()
        result = self.prepare(Path('manifest.json'), cwd=caller)
        self.assertEqual(result.returncode, 0, repr(result.stderr))
        self.assertEqual(existing.read_bytes(), b'preserve repository manifest')
        self.assertTrue((caller / 'manifest.json').is_file())

    def test_manifest_created_during_scan_is_preserved(self):
        output = self.root / 'out/manifest.json'
        helper = self.root / 'tools/wf_hash.ps1'
        quoted = str(output).replace("'", "''")
        helper.write_text(helper.read_text(encoding='utf-8').replace(
            '  $stream=$null',
            f"  [IO.File]::WriteAllText('{quoted}', 'concurrent review')\n  $stream=$null"),
            encoding='utf-8')
        self.assertNotEqual(self.prepare(output).returncode, 0)
        self.assertEqual(output.read_bytes(), b'concurrent review')

    def test_unborn_head_does_not_produce_a_ready_manifest(self):
        self.git('symbolic-ref', 'HEAD', 'refs/heads/unborn-fixture')
        output = self.root / 'out/manifest.json'
        self.assertNotEqual(self.prepare(output).returncode, 0)
        self.assertFalse(output.exists())

    def test_validation_source_manifest_uses_the_same_unicode_paths(self):
        (self.root / 'src').mkdir()
        source = self.root / 'src/日本語.h'
        source.write_bytes(b'fixture source\n')
        self.git('add', '--', 'src/日本語.h')
        # Load only this function; do not launch validation builds or publish evidence.
        script = self.root / 'out/source-manifest.ps1'
        script.write_text("""param([string]$Repo,[string]$Tools)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Set-Location -LiteralPath $Repo
. (Join-Path $Tools 'wf_hash.ps1')
if (Test-Path -LiteralPath (Join-Path $Tools 'wf_git.ps1')) { . (Join-Path $Tools 'wf_git.ps1') }
$tokens=$null
$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $Tools 'run_release_validation.ps1'),[ref]$tokens,[ref]$errors)
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Source-Manifest'},$true)
. ([scriptblock]::Create($function.Extent.Text))
Source-Manifest | ConvertTo-Json
""", encoding='utf-8')
        result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(script), '-Repo', str(self.root),
                                 '-Tools', str(ROOT / 'tools')], capture_output=True,
                                env=self.environment)
        self.assertEqual(result.returncode, 0, repr(result.stderr))
        files = {item['path']: item for item in json.loads(result.stdout.decode('utf-8-sig'))}
        self.assertEqual(files['src/日本語.h']['sha256'].lower(), hashlib.sha256(source.read_bytes()).hexdigest())


@unittest.skipUnless(sys.platform == 'win32', 'Windows Git fixture environment')
class GitEnvironmentIsolation(unittest.TestCase):
    def test_inherited_git_directory_cannot_redirect_fixture_commands(self):
        parent = ROOT / 'out/baseline-tests'
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            decoy = Path(directory).resolve() / 'decoy.git'
            env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
            env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
                       GIT_CONFIG_NOSYSTEM='1')
            subprocess.run(['git', 'init', '--bare', '-q', str(decoy)],
                           check=True, capture_output=True, env=env)
            before = (decoy / 'config').read_bytes()
            with patch.dict(os.environ, GIT_DIR=str(decoy), GIT_WORK_TREE=str(decoy.parent)):
                fixture = BaselineManifest()
                try:
                    fixture.setUp()
                    fixture.git('rev-parse', '--verify', 'HEAD')
                finally:
                    fixture.doCleanups()
            self.assertEqual((decoy / 'config').read_bytes(), before)
            self.assertFalse(any((decoy / 'refs/heads').iterdir()))


if __name__ == '__main__':
    unittest.main()
