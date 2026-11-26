param([Parameter(Mandatory)][string]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
Initialize-WfToolchain
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = (Resolve-Path -LiteralPath $Build).Path
$root = Join-Path $buildRoot ('dircache-mutants-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$original = [IO.File]::ReadAllText((Join-Path $repo 'src/ui/wf_dircache.c'))
$tests = [IO.File]::ReadAllText((Join-Path $repo 'tests/test_dircache.c'))
$mutants = @(
    @('forward5', 'side ? 3 : 5', 'side ? 3 : 4'),
    @('backward3', 'side ? 3 : 5', 'side ? 2 : 5'),
    @('identity', 'a->volume == b->volume &&', 'a->volume == b->volume ||'),
    @('freshness', 'app->now_ms - e->loaded <= WF_DIRCACHE_TTL', 'true'),
    @('generation', 'gen != c->flight', 'gen == 0'),
    @('partial', '!dir || dir->truncated', '!dir'))
$compiler = (Get-Command clang-cl.exe | Select-Object -First 1).Source
foreach ($mutation in $mutants) {
    $name, $old, $new = $mutation
    if ($original.IndexOf($old) -lt 0) { throw "Missing mutation anchor: $name" }
    $dir = Join-Path $root $name
    New-Item -ItemType Directory -Path $dir | Out-Null
    $source = $original.Replace($old, $new)
    $source = [regex]::Replace($source, '#include "([^"]+)"', {
        param($match)
        '#include "' + ([IO.Path]::GetFullPath((Join-Path "$repo/src/ui" $match.Groups[1].Value))).Replace('\','/') + '"'
    })
    $file = Join-Path $dir 'mutant.c'
    [IO.File]::WriteAllText($file, $source)
    $driver = Join-Path $dir 'driver.c'
    [IO.File]::WriteAllText($driver, $tests.Replace('../src/ui/wf_dircache.c', $file.Replace('\','/')))
    $exe = Join-Path $dir 'test.exe'
    & $compiler /nologo /O2 /MD /std:c17 /utf-8 /DUNICODE /D_UNICODE "/I$repo/src" "/I$repo/tests" "/Fo$dir/" "/Fe$exe" $driver /link "$buildRoot/wfcore.lib" advapi32.lib user32.lib shell32.lib ole32.lib uuid.lib *> (Join-Path $dir 'build.log')
    if ($LASTEXITCODE) { throw "Mutant compile failed (not a kill): $name" }
    $receipt = Join-Path $dir 'process.json'
    & "$buildRoot/p0_run.exe" 15000 $exe (Join-Path $dir 'test.log') (Join-Path $dir 'error.log') $receipt
    if ($LASTEXITCODE) { throw "Supervisor failed: $name" }
    $result = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    $assertion = Select-String -LiteralPath (Join-Path $dir 'test.log') -SimpleMatch 'FAIL ' -Quiet
    if ($result.state -ne 0 -or $result.exit_code -ne 1 -or !$assertion) {
        throw "Mutant survived or did not finish with assertion failure: $name, $($result | ConvertTo-Json -Compress)"
    }
    "KILLED $name"
}
"PASS: $($mutants.Count) cache mutants, source retained at $root"
