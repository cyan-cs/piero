# Build, run, and preserve P0-S04/S05/S06 without overwriting an earlier run.
param(
  [ValidateSet('clang','msvc')][string]$Toolchain = 'clang',
  [string]$RunId
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$RunId) { $RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-storage-' + $Toolchain + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) }
if ($RunId -notmatch '^[A-Za-z0-9_-]+$') { throw 'Unsafe run ID' }
$config = if ($Toolchain -eq 'clang') { 'Debug' } else { 'Release' }
$compiler = (Get-Command $(if($Toolchain -eq 'clang'){'clang-cl'}else{'cl'}) | Select-Object -First 1).Source
$ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja | Select-Object -First 1).Source }
$build = Join-Path $repo ('out/final-' + $Toolchain + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$dirs = @('P0-S04','P0-S05','P0-S06') | ForEach-Object { Join-Path $repo "p0-results/$_/$RunId" }
foreach ($dir in $dirs) {
  foreach ($name in @('run.json','environment.json','cases.csv','validation.json','artifacts/configure.log')) {
    if (Test-Path -LiteralPath (Join-Path $dir $name)) { throw "Refusing to overwrite evidence: $dir/$name" }
  }
  New-Item -ItemType Directory -Force -Path (Join-Path $dir 'artifacts') | Out-Null
}
$primary = Join-Path $dirs[2] 'artifacts'
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$Log) {
  & $Exe @Arguments *> $Log
  $code = $LASTEXITCODE
  Write-Output "$(Split-Path -Leaf $Exe): exit=$code log=$Log"
  if ($code -ne 0) { throw "Command failed ($code); see $Log" }
}
Invoke-Logged 'cmake' @('-S',$repo,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_C_COMPILER=$compiler","-DCMAKE_BUILD_TYPE=$config") (Join-Path $primary 'configure.log')
Invoke-Logged 'cmake' @('--build',$build) (Join-Path $primary 'build.log')
Invoke-Logged 'ctest' @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $primary 'ctest.xml')) (Join-Path $primary 'ctest.log')
Invoke-Logged (Join-Path $build 'test_storage_win.exe') @('--evidence',(Join-Path $repo 'p0-results'),$RunId) (Join-Path $primary 'storage.log')
Invoke-Logged (Join-Path $build 'test_owned_faults.exe') @('--evidence',(Join-Path $primary 'faults.csv')) (Join-Path $primary 'faults.log')
Invoke-Logged (Join-Path $build 'test_owned_record.exe') @() (Join-Path $primary 'unit-record.log')

$mutation = @(Get-ChildItem -LiteralPath $build -Directory -Filter 'mutations-*')
if ($mutation.Count -ne 1) { throw 'Expected one CTest mutation run' }
$mutationResults = Get-Content -LiteralPath (Join-Path $mutation[0].FullName 'mutations.json') -Raw | ConvertFrom-Json
if (@($mutationResults | Where-Object result -ne 'KILLED').Count) { throw 'Surviving mutation' }
$mutationEvidence = Join-Path $primary 'mutations'
New-Item -ItemType Directory -Path $mutationEvidence | Out-Null
Copy-Item -LiteralPath (Join-Path $mutation[0].FullName 'mutations.json') -Destination $mutationEvidence
foreach ($m in $mutationResults) {
  $target = Join-Path $mutationEvidence $m.name
  New-Item -ItemType Directory -Path $target | Out-Null
  Copy-Item -LiteralPath (Join-Path $m.directory 'test.log'),(Join-Path $m.directory 'build.log') -Destination $target
}
Copy-Item -LiteralPath (Join-Path $build 'Testing/Temporary/LastTest.log') -Destination (Join-Path $primary 'ctest-detail.log')
$sources = @('.gitignore','CMakeLists.txt','src/fs/owned_delete.c','src/fs/owned_delete.h',
    'src/fs/owned_common.c','src/fs/owned_payload.c','src/fs/owned_committed.c','src/fs/owned_local.c','src/fs/owned_live.c','src/fs/owned_log.c','src/fs/owned_internal.h',
    'tools/p0/owned_test_sources.ps1','tests/wf_owned_fault_shim.h','tests/wf_recovery_fault_shim.h',
  'src/path/wf_canonical_win.c','src/path/wf_canonical_win.h','tests/test_owned_record.c',
  'tests/test_storage_win.c','tests/test_owned_faults.c','tests/test_xfer_win.c',
  'tools/p0/test_storage_mutations.ps1','tools/p0/test_storage_evidence.ps1','tools/p0/run_storage.ps1')
$manifest = $sources | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-WfFileSha256 (Join-Path $repo $_))} }
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $primary 'source-manifest.json') -Encoding utf8
[xml]$junit = Get-Content -LiteralPath (Join-Path $primary 'ctest.xml') -Raw
$skipped = @($junit.testsuite.testcase | Where-Object { $_.status -eq 'notrun' -or $_.skipped }).Count
$validation = [ordered]@{
  run_id=$RunId; toolchain=$Toolchain; configuration=$config; compiler=$compiler; ninja=$ninja; build=$build;
  warning_policy='/W4 /WX'; build_exit=0; ctest_exit=0; ctest_total=[int]$junit.testsuite.tests;
  ctest_failures=[int]$junit.testsuite.failures; ctest_skipped=$skipped;
  ctest_passed=([int]$junit.testsuite.tests - $skipped - [int]$junit.testsuite.failures);
  all_ctest_pass_requirement=$(if($skipped){'BLOCKED'}else{'PASS'});
  blocked_reason=$(if($skipped){'xfer_win: runner cannot open the profile ancestor; safe IO_ERROR checked, remaining integration cases skipped.'}else{''});
  mutants_killed=$mutationResults.Count; production_copy_stop='RETAINED'; commits_created=0;
}
$matrix = [ordered]@{
  measured='Actual OS/filesystem/compiler recorded in environment.json. No other matrix cell is PASS.';
  windows10_22h2='NOT_TESTED'; ReFS='NOT_TESTED'; exFAT='NOT_TESTED'; SMB='NOT_TESTED';
  USB_drive_letter_reassignment='NOT_TESTED'; mounted_volume='NOT_TESTED'; cloud_placeholder='NOT_TESTED';
  symbolic_link='BLOCKED: ERROR_PRIVILEGE_NOT_HELD (1314)'; directory_junction='TESTED: refused';
  readonly_legacy_fallback='TESTED: Ex unsupported injected; real FileBasicInfo + legacy disposition on verified handle';
  all_disposition_unsupported='TESTED: injected errors; RECOVERY_REQUIRED; no path-delete fallback';
  power_loss='NOT_TESTED (process crash only)';
  non_payload_owned_kinds='BLOCKED: safely refused until their kind-specific authority exists';
  missing_payload='ID absence may return error 87 on NTFS: MANUAL, no delete, no ABORTED/COMMITTED inference';
  DR_001='DRAFT; formal design unchanged'; hypotheses='OPEN; no final H-xx decision'
}
foreach ($dir in $dirs) {
  $validation | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dir 'validation.json') -Encoding utf8
  $matrix | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dir 'matrix.json') -Encoding utf8
  if ($dir -ne $dirs[2]) {
    Copy-Item -LiteralPath (Join-Path $primary 'source-manifest.json'),(Join-Path $primary 'configure.log'),
      (Join-Path $primary 'build.log'),(Join-Path $primary 'ctest.log'),(Join-Path $primary 'ctest.xml'),
      (Join-Path $primary 'storage.log') -Destination (Join-Path $dir 'artifacts')
  }
}
Write-Output "run_id=$RunId; tests=$($validation.ctest_passed) PASS, $skipped SKIP; mutants=$($mutationResults.Count) KILLED"
