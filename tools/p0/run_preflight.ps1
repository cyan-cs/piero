# Fresh builds and append-only evidence for the independent S08-S10 PoC.
param(
  [ValidateSet('clang','msvc')][string]$Toolchain = 'clang',
  [string]$RunId
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
. (Join-Path $PSScriptRoot 'wf_ctest_results.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$RunId) { $RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-s08s10-' + $Toolchain + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) }
if ($RunId -notmatch '^[A-Za-z0-9_-]+$') { throw 'Unsafe run ID' }
$config = if ($Toolchain -eq 'clang') { 'Debug' } else { 'Release' }
$compiler = (Get-Command $(if($Toolchain -eq 'clang'){'clang-cl'}else{'cl'}) | Select-Object -First 1).Source
$ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $ninja)) { throw 'VS Ninja is required in this restricted runner' }
$build = Join-Path $repo ('out/preflight-final-' + $Toolchain + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$dirs = @('P0-S08','P0-S09','P0-S10') | ForEach-Object { Join-Path $repo "p0-results/$_/$RunId" }
foreach ($dir in $dirs) {
  if (Test-Path -LiteralPath $dir) { throw "Refusing to overwrite evidence: $dir" }
}
foreach ($dir in $dirs) { New-Item -ItemType Directory -Path (Join-Path $dir 'artifacts') | Out-Null }
$primary = Join-Path $dirs[2] 'artifacts'
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$Log, [int]$Expected = 0) {
  $saved = $ErrorActionPreference
  try { $ErrorActionPreference = 'Continue'; & $Exe @Arguments *> $Log; $code = $LASTEXITCODE }
  finally { $ErrorActionPreference = $saved }
  Write-Output "$(Split-Path -Leaf $Exe): exit=$code log=$Log"
  if ($code -ne $Expected) { throw "Expected $Expected, got $code; see $Log" }
}
try {
  Invoke-Logged 'cmake' @('-S',$repo,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_C_COMPILER=$compiler","-DCMAKE_BUILD_TYPE=$config") (Join-Path $primary 'configure.log')
  Invoke-Logged 'cmake' @('--build',$build) (Join-Path $primary 'build.log')
  Invoke-Logged 'ctest' @('--test-dir',$build,'--show-only=json-v1') (Join-Path $primary 'ctest-manifest.json')
  Invoke-Logged 'ctest' @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $primary 'ctest.xml')) (Join-Path $primary 'ctest.log')
  $ctestPassed = Read-WfCtestPassed (Join-Path $primary 'ctest-manifest.json') (Join-Path $primary 'ctest.xml')
  Invoke-Logged (Join-Path $build 'p0_s08s10.exe') @('--evidence',(Join-Path $repo 'p0-results'),$RunId) (Join-Path $primary 'measurement.log')
  Invoke-Logged (Join-Path $build 'test_source_guard_faults.exe') @() (Join-Path $primary 'guard-faults.log')
  Invoke-Logged (Join-Path $build 'test_xfer_win.exe') @('--literal') (Join-Path $primary 'literal-ancestor-blocked.log') 77

  # Duplicate evidence must be rejected without modifying a single existing byte.
  $evidenceFiles = foreach ($dir in $dirs) { foreach ($name in @('cases.csv','environment.json','run.json','alias.json')) { Join-Path $dir $name } }
  $before = @($evidenceFiles | ForEach-Object { (Get-WfFileSha256 $_) })
  Invoke-Logged (Join-Path $build 'p0_s08s10.exe') @('--evidence',(Join-Path $repo 'p0-results'),$RunId) (Join-Path $primary 'duplicate-rejection.log') 2
  $after = @($evidenceFiles | ForEach-Object { (Get-WfFileSha256 $_) })
  if (Compare-Object $before $after -SyncWindow 0) { throw 'Duplicate run altered existing evidence' }

  $kills = @{}
  foreach ($group in @(@{pattern='preflight-mutants-*';name='preflight-mutations';count=18},@{pattern='mutations-*';name='storage-mutations';count=14})) {
    $mutation = @(Get-ChildItem -LiteralPath $build -Directory -Filter $group.pattern)
    if ($mutation.Count -ne 1) { throw "Expected exactly one $($group.name) run" }
    $results = Get-Content -LiteralPath (Join-Path $mutation[0].FullName 'mutations.json') -Raw | ConvertFrom-Json
    if ($results.Count -ne $group['count'] -or @($results | Where-Object result -ne 'KILLED').Count) { throw 'Mutation count/survivor failure' }
    $target = Join-Path $primary $group.name
    New-Item -ItemType Directory -Path $target | Out-Null
    Copy-Item -LiteralPath (Join-Path $mutation[0].FullName 'mutations.json') -Destination $target
    foreach ($m in $results) {
      $caseDir = Join-Path $target $m.name; New-Item -ItemType Directory -Path $caseDir | Out-Null
      Copy-Item -LiteralPath (Join-Path $m.directory 'test.log'),(Join-Path $m.directory 'build.log') -Destination $caseDir
    }
    $kills[$group.name] = $results.Count
  }
  Copy-Item -LiteralPath (Join-Path $build 'Testing/Temporary/LastTest.log') -Destination (Join-Path $primary 'ctest-detail.log')
  $sources = @('CMakeLists.txt','src/xfer/wf_xfer.h','src/xfer/wf_xfer_paths.c','src/xfer/wf_preflight.c','src/xfer/wf_preflight_paths.c','src/xfer/wf_preflight_walk.c','src/xfer/wf_preflight_internal.h','src/xfer/wf_probe_win.c',
    'tests/test_xfer.c','tests/test_xfer_win.c','tests/test_preflight_batch.c','tests/test_source_guard_win.c','tests/test_source_guard_faults.c',
    'tools/p0/p0_area.c','tools/p0/p0_lab.h','tools/p0/p0_lab.c','tools/p0/p0_source_guard.h','tools/p0/p0_source_guard.c',
    'tools/p0/p0_s08s10.c','tools/p0/test_preflight_mutations.ps1','tools/p0/run_preflight.ps1','tools/p0/wf_ctest_results.ps1')
  @($sources | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-WfFileSha256 (Join-Path $repo $_))} }) |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $primary 'source-manifest.json') -Encoding utf8
  if (Select-String -LiteralPath (Join-Path $primary 'build.log') -Pattern '\bwarning [A-Z]*\d+:|\berror [A-Z]*\d+:' -Quiet) { throw 'Compiler diagnostic in build log' }
  $caseCounts = @()
  for ($i = 0; $i -lt 3; $i++) {
    $cases = @(Import-Csv -LiteralPath (Join-Path $dirs[$i] 'cases.csv'))
    if ($cases.Count -ne @(13,9,13)[$i] -or @($cases | Where-Object result -ne 'PASS').Count) { throw 'Measurement case count/failure' }
    $caseCounts += $cases.Count
    foreach ($name in @('environment.json','run.json','alias.json')) { $null = Get-Content -LiteralPath (Join-Path $dirs[$i] $name) -Raw | ConvertFrom-Json }
    $summary = Get-Content -LiteralPath (Join-Path $dirs[$i] 'run.json') -Raw | ConvertFrom-Json
    if ($summary.result -ne 'PASS') { throw 'Measurement run summary is not PASS' }
  }
  $validation = [ordered]@{
    run_id=$RunId;toolchain=$Toolchain;configuration=$config;compiler=$compiler;ninja=$ninja;build=$build;
    clean_build=$true;warning_policy='/W4 /WX';warnings=0;errors=0;build_exit=0;ctest_exit=0;
    ctest_total=$ctestPassed;ctest_passed=$ctestPassed;ctest_failures=0;ctest_skipped=0;measured_case_counts=$caseCounts;
    mutations_killed=$kills;duplicate_run_exit=2;duplicate_evidence_unchanged=$true;
    literal_profile_ancestor='BLOCKED: actual exit 77; safe IO_ERROR asserted; see literal-ancestor-blocked.log';
    dedicated_alias_xfer_win='PASS: same checks, fresh owned NTFS root, verified DOS mapping, no existing ACL changes';
    production_copy_stop='RETAINED';commits_created=0
  }
  $matrix = [ordered]@{
    measured='Only environment.json OS/filesystem/toolchain was measured. Injected tests are explicitly identified in CSV.';
    windows10_22h2='NOT_TESTED';ReFS='NOT_TESTED';exFAT='NOT_TESTED';SMB='NOT_TESTED';
    real_cloud_placeholder='NOT_TESTED: policy branch tested with injected CLOUD metadata; no hydration performed';
    actual_disk_exhaustion='NOT_TESTED: real available bytes compared with deliberately large configured reserve';
    reserve_256MiB='PROVISIONAL CONFIGURATION: no claim that this margin is empirically sufficient';
    metadata_immutability='NOT_GUARANTEED: FILE_SHARE_READ is a sharing guard, not a filesystem snapshot';
    CopyFileEx_path_TOCTOU='OPEN: guard retained through PoC CopyFileExW; later path resolution is not proven race-free';
    directory_snapshot='NOT_IMPLEMENTED: ordinary-file guard PoC only';
    transparent_reparse_directory='BLOCKED: safely refused until complete policy-compliant tree scan/hydration exists';
    long_paths='Sampled successful extended-W API ranges are in CSV; 32766 policy ceiling is not a measured universal maximum';
    volume_matrix='NOT_TESTED outside measured local NTFS; unsupported/unknown capabilities fail preflight safely';
    DR_001='DRAFT; PROGRESS_STOP open; no design change';hypotheses='OPEN; no final H-xx decision'
  }
  foreach ($dir in $dirs) {
    $validation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'validation.json') -Encoding utf8
    $matrix | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'matrix.json') -Encoding utf8
    if ($dir -ne $dirs[2]) {
      foreach ($name in @('source-manifest.json','configure.log','build.log','ctest.log','ctest.xml','ctest-manifest.json','measurement.log','guard-faults.log','literal-ancestor-blocked.log')) {
        Copy-Item -LiteralPath (Join-Path $primary $name) -Destination (Join-Path $dir 'artifacts')
      }
    }
  }
  Write-Output "run_id=$RunId; tests=$ctestPassed PASS, 0 FAIL, 0 SKIP; mutations=18 new + 14 existing KILLED; cases=$($caseCounts -join '/')"
} catch {
  foreach ($dir in $dirs) {
    $failure = Join-Path $dir 'run.failure.json'
    if (!(Test-Path -LiteralPath $failure)) {
      [ordered]@{run_id=$RunId;result='FAIL';reason=$_.Exception.Message;build=$build;production_copy_stop='RETAINED'} |
        ConvertTo-Json | Set-Content -LiteralPath $failure -Encoding utf8
    }
  }
  throw
}
