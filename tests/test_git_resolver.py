"""Git executable selection is scalar and follows PATH order on both shells."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'Windows application lookup')
class GitResolver(unittest.TestCase):
    def test_selection_and_execution(self):
        base = ROOT / 'out/git-resolver-tests'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            directory = Path(directory)
            # Lookup needs real files, but must not execute these placeholders.
            paths = [directory / 'first space', directory / '\u65e5\u672c\u8a9e']
            for path in paths:
                path.mkdir()
                (path / 'git.exe').write_bytes(b'lookup fixture')
            driver = directory / 'driver.ps1'
            driver.write_text("""param([string]$Helper,[string]$First,[string]$Second)
$ErrorActionPreference='Stop'
. $Helper
$oldPath=$env:PATH
$real=Get-WfGitExecutable
try {
  $env:PATH=$First
  if((Get-WfGitExecutable) -ne (Join-Path $First 'git.exe')){throw 'single match'}
  $env:PATH=$First+';'+$Second
  $selected=Get-WfGitExecutable
  if($selected -isnot [string] -or $selected -ne (Join-Path $First 'git.exe')){throw 'multiple matches'}
  $env:PATH=$Second+';'+$First
  if((Get-WfGitExecutable) -ne (Join-Path $Second 'git.exe')){throw 'PATH order/unicode'}
  foreach($letter in [char[]]'DEFGHIJKLMNOPQRSTUVWXYZ') {
    $drive=[string]$letter+':\\'
    if(![IO.Directory]::Exists($drive)) {
      $env:PATH=$drive+';'+$Second
      if((Get-WfGitExecutable) -ne (Join-Path $Second 'git.exe')){throw 'missing drive prevented lookup'}
      break
    }
  }
  Push-Location -LiteralPath $First
  try {
    $env:PATH='.;'+$Second
    if((Get-WfGitExecutable) -ne (Join-Path $Second 'git.exe')){throw 'relative PATH selected cwd'}
  } finally { Pop-Location }
  $env:PATH=''
  $refused=$false
  try { $null=Get-WfGitExecutable } catch { $refused=$true }
  if(!$refused){throw 'missing Git accepted'}
} finally { $env:PATH=$oldPath }
$text=Read-WfGit $PWD '--version'
if($text -notmatch '^git version '){throw 'UTF-8 process output'}
$refused=$false
try { $null=Read-WfGit $PWD '--not-a-real-option' } catch { $refused=$true }
if(!$refused){throw 'nonzero Git exit accepted'}
'PASS'
""", encoding='utf-8')
            shells = [shutil.which(name) for name in ('powershell.exe', 'pwsh.exe')]
            self.assertTrue(any(shells))
            for shell in filter(None, shells):
                with self.subTest(shell=Path(shell).name):
                    result = subprocess.run([
                        shell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                        '-File', str(driver), '-Helper', str(ROOT / 'tools/wf_git.ps1'),
                        '-First', str(paths[0]), '-Second', str(paths[1])],
                        cwd=ROOT, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))


if __name__ == '__main__':
    unittest.main()
