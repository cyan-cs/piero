"""Cache misses, verified hits, tampering and single-source compile routing."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'win32', 'Windows PowerShell cache helpers')
class CacheTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/ci-cache-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.folder = Path(tempfile.mkdtemp(dir=parent)).resolve()

    def run_ps(self, body, *arguments):
        script = self.folder / 'driver.ps1'
        script.write_text("param([string]$Repo,[string]$Directory)\n"
                          "$ErrorActionPreference='Stop'\n" + body, encoding='utf-8-sig')
        result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(script), str(ROOT), str(self.folder), *arguments],
                                capture_output=True, env={k: v for k, v in os.environ.items()
                                                         if k != 'PIERO_COMPILER_CACHE'})
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return json.loads(result.stdout.decode().split('RESULT_JSON:')[-1])

    def test_miss_downloads_hit_verifies_and_corruption_is_rejected(self):
        digest = hashlib.sha256(b'release fixture').hexdigest()
        result = self.run_ps("""
. (Join-Path $Repo 'tools/wf_download.ps1')
$script:downloads=0
function Invoke-WebRequest($Uri,$OutFile,[switch]$UseBasicParsing){
 $script:downloads++
 [IO.File]::WriteAllText($OutFile,'release fixture')
}
$archive=Join-Path $Directory 'release.tar.zst'
Get-WfVerifiedDownload 'https://example.invalid/release' $archive 'DIGEST' | Out-Null
Get-WfVerifiedDownload 'https://example.invalid/release' $archive 'DIGEST' | Out-Null
[IO.File]::WriteAllText($archive,'damaged')
$rejected=$false
try {Get-WfVerifiedDownload 'https://example.invalid/release' $archive 'DIGEST' | Out-Null}
catch {$rejected=$_.Exception.Message -like '*checksum mismatch*'}
'RESULT_JSON:'+ (ConvertTo-Json -Compress @{downloads=$script:downloads;rejected=$rejected})
""".replace('DIGEST', digest))
        self.assertEqual(result, {'downloads': 1, 'rejected': True})

    def test_cache_wraps_each_source_but_never_linking_and_compile_failure_stops(self):
        # The fixture logs arguments, avoiding network or an installed sccache.
        (self.folder / 'compiler.ps1').write_text("""
$args | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:PROBE_CALLS
if($args -contains $env:PROBE_FAIL_SOURCE){$global:LASTEXITCODE=17}else{$global:LASTEXITCODE=0}
""", encoding='utf-8-sig')
        (self.folder / 'cache.ps1').write_text("""
$compiler=$args[0]
$forward=@($args | Select-Object -Skip 1)
if($forward -notcontains '/c'){throw 'Link command entered the compiler cache'}
& $compiler @forward
""", encoding='utf-8-sig')
        result = self.run_ps("""
. (Join-Path $Repo 'tools/p0/wf_compile.ps1')
$compiler=Join-Path $Directory 'compiler.ps1'
$env:PIERO_COMPILER_CACHE=Join-Path $Directory 'cache.ps1'
$env:PROBE_CALLS=Join-Path $Directory 'calls.jsonl'
$a=Join-Path $Directory 'same.c'
$b=Join-Path $Directory 'nested/same.c'
$arguments=@('/nologo','/MDd','/DTEST',"/Fo$Directory/",('/Fe'+(Join-Path $Directory 'test.exe')),$a,$b,'/link','library.lib')
$ok=Invoke-WfMutationBuild $compiler $arguments $Directory
$calls=[IO.File]::ReadAllLines($env:PROBE_CALLS)
$env:PROBE_CALLS=Join-Path $Directory 'failed-calls.jsonl'
$env:PROBE_FAIL_SOURCE=$b
$failed=Invoke-WfMutationBuild $compiler $arguments $Directory
$failureCalls=@(Get-Content -LiteralPath $env:PROBE_CALLS)
'RESULT_JSON:'+ (ConvertTo-Json -Depth 5 -Compress @{ok=$ok;failed=$failed;calls=$calls;failure_calls=$failureCalls.Count})
""")
        self.assertEqual((result['ok'], result['failed'], result['failure_calls']), (0, 17, 2))
        a, b, link = [json.loads(call) for call in result['calls']]
        self.assertIn('/MDd', a)
        self.assertIn('/DTEST', b)
        self.assertTrue(any(x.endswith('0-same.obj') for x in a))
        self.assertTrue(any(x.endswith('1-same.obj') for x in b))
        self.assertNotIn('/c', link)
        self.assertIn('library.lib', link)
        self.assertEqual(sum(x.endswith('.obj') for x in link), 2)


if __name__ == '__main__':
    unittest.main()
