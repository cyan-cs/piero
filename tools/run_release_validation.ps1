# Fresh, append-only validation of the integrated development application.
# A passing build is not a release-gate approval.
param(
  [ValidateSet('clang','msvc')][string]$Toolchain = 'clang',
  [string]$RunId
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wf_hash.ps1')
. (Join-Path $PSScriptRoot 'wf_git.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'wf_vs.ps1')
$vs = Find-WfVisualStudio
if (!$env:VCToolsInstallDir) {
  Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
if (!$RunId) { $RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-integration-' + $Toolchain + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) }
if ($RunId -notmatch '^[A-Za-z0-9_-]{1,85}$') { throw 'Unsafe run ID' }
$config = if ($Toolchain -eq 'clang') { 'Debug' } else { 'Release' }
$compiler = (Get-Command $(if ($Toolchain -eq 'clang') {'clang-cl'} else {'cl'})).Source
$ninja = Find-WfNinja $vs
$build = Join-Path $repo ('out/' + $RunId)
$evidence = Join-Path $repo ('p0-results/FOUNDATION/' + $RunId)
if ((Test-Path -LiteralPath $build) -or (Test-Path -LiteralPath $evidence)) { throw 'Refusing an existing build/evidence directory' }
$artifacts = Join-Path $evidence 'artifacts'
New-Item -ItemType Directory -Path $artifacts | Out-Null
function Source-Manifest {
  $listed = Read-WfGit $repo 'ls-files -z --cached --others --exclude-standard -- src tests tools cmake'
  $paths = @('CMakeLists.txt','.gitignore','.gitattributes','README.md') +
    @($listed.Split([char[]]@([char]0), [StringSplitOptions]::RemoveEmptyEntries))
  @($paths | Sort-Object -Unique -CaseSensitive | ForEach-Object {
    [pscustomobject]@{path=$_;sha256=(Get-WfFileSha256 (Join-Path $repo $_))}
  })
}
function Invoke-Logged([string]$Exe,[string[]]$Arguments,[string]$Name,[int]$ExpectedExit=0) {
  $log = Join-Path $artifacts $Name
  $saved = $ErrorActionPreference
  try { $ErrorActionPreference='Continue'; & $Exe @Arguments *> $log; $code=$LASTEXITCODE }
  finally { $ErrorActionPreference=$saved }
  Write-Output "$(Split-Path -Leaf $Exe): exit=$code log=$log"
  if ($code -ne $ExpectedExit) { throw "Expected $ExpectedExit, got $code; see $log" }
}
# Every baseline test must exist exactly once and have run and passed.
function Assert-CtestBaseline($Cases,[string[]]$Baseline) {
  foreach ($name in $Baseline) {
    $case = @($Cases | Where-Object name -eq $name)
    if ($case.Count -ne 1 -or $case[0].failure -or $case[0].skipped -or $case[0].status -ne 'run') { throw "Baseline test not passing: $name" }
  }
}

# Every mutation group the build produced must have killed all its mutants;
# their logs are copied into the evidence. Returns one summary per group.
function Get-MutationEvidence([string]$Build,[string]$Artifacts) {
  $groups = @()
  foreach ($dir in @(Get-ChildItem -LiteralPath $Build -Directory)) {
    $file = Join-Path $dir.FullName 'mutations.json'
    if (!(Test-Path -LiteralPath $file)) { continue }
    # Windows PowerShell emits a JSON array as one pipeline object. Assign
    # directly so its elements, rather than an extra wrapper, are inspected.
    $mutants = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if (!$mutants.Count -or @($mutants | Where-Object result -ne 'KILLED').Count) { throw "Mutation failed: $file" }
    $target = Join-Path $Artifacts $dir.Name
    New-Item -ItemType Directory -Path $target | Out-Null
    Copy-Item -LiteralPath $file -Destination $target
    foreach ($mutant in $mutants) {
      $subdir = Join-Path $target $mutant.name
      New-Item -ItemType Directory -Path $subdir | Out-Null
      Copy-Item -LiteralPath (Join-Path $mutant.directory 'build.log'),(Join-Path $mutant.directory 'test.log') -Destination $subdir
    }
    $groups += [pscustomobject]@{group=$dir.Name;killed=$mutants.Count;survived=0}
  }
  if ($groups.Count -lt 8) { throw 'Missing required mutation groups' }
  $groups
}

# Logs of earlier red/green development runs, kept with the evidence when present.
function Copy-DevelopmentRegressions([string]$Repo,[string]$Artifacts) {
  $redLogs=@('out/worker-lifetime-red.log','out/hash-dependency-red-confirmed.log','out/hash-dependency-green-final.log',
    'out/preview-error-red.log','out/validation-array-regression.json','out/storage-next-agent/red-partial-state.log','out/storage-next-agent/red-get-diagnostic-2.log',
    'out/release-name/red.log','out/release-name/edit-red.log','out/release-name/config-win-red.log','out/release-name/startup-red.log','out/release-name/cli-red.log')
  $redDir=Join-Path $Artifacts 'development-regressions'
  New-Item -ItemType Directory -Path $redDir | Out-Null
  foreach($redLog in $redLogs){
    if(Test-Path -LiteralPath (Join-Path $Repo $redLog)){
      Copy-Item -LiteralPath (Join-Path $Repo $redLog) -Destination (Join-Path $redDir ($redLog.Replace('/','_')))
    }
  }
}

try {
  Push-Location -LiteralPath $repo
  $before = @(Source-Manifest)
  $before | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $artifacts 'source-manifest.json')
  Invoke-Logged cmake @('-S',$repo,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_C_COMPILER=$compiler","-DCMAKE_BUILD_TYPE=$config") 'configure.log'
  Invoke-Logged cmake @('--build',$build) 'build.log'
  $productBuild=$build+'-product'
  Invoke-Logged cmake @('-S',$repo,'-B',$productBuild,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_C_COMPILER=$compiler","-DCMAKE_BUILD_TYPE=$config",'-DBUILD_TESTING=OFF') 'product-configure.log'
  Invoke-Logged cmake @('--build',$productBuild) 'product-build.log'
  if(Test-Path -LiteralPath (Join-Path $productBuild 'p0_env.exe')){throw 'P0 target leaked into product-only build'}
  Invoke-Logged ctest @('--test-dir',$build,'--output-on-failure','--output-junit',(Join-Path $artifacts 'ctest.xml')) 'ctest.log'
  Copy-Item -LiteralPath (Join-Path $build 'Testing/Temporary/LastTest.log') -Destination (Join-Path $artifacts 'ctest-detail.log')
  [xml]$xml = Get-Content -LiteralPath (Join-Path $artifacts 'ctest.xml') -Raw
  $cases = @($xml.testsuite.testcase)
  $skipped = @($cases | Where-Object { $_.status -eq 'notrun' -or $_.skipped }).Count
  $failed = [int]$xml.testsuite.failures
  if ($failed) { throw 'CTest reported failures' }
  $baseline = @('owned_record','text','frame','layout','path','dir','sel','debounce','confirm','app','xfer','slot','preflight_batch','enum_win','async_win','xfer_win','source_guard_win','source_guard_faults','p0_s08s10','p0_area','storage_win','owned_faults','preflight_mutations','storage_evidence','storage_mutations','ci_banned_apis')
  Assert-CtestBaseline $cases $baseline
  $diagnostics = @(Select-String -LiteralPath (Join-Path $artifacts 'build.log'),(Join-Path $artifacts 'product-build.log') -Pattern '\bwarning [A-Z]*\d+:|\berror [A-Z]*\d+:|\bwarning:|\berror:')
  if ($diagnostics.Count) { throw 'Compiler diagnostics in clean build' }
  $mutationGroups = @(Get-MutationEvidence $build $artifacts)
  # Independently retained per-P0 runs use the just-built executable hashes.
  Invoke-Logged powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'tools/p0/run_recovery.ps1'),'-Build',$build,'-RunId',$RunId,'-LiveIterations','100') 'recovery-measurement.log'
  Invoke-Logged (Join-Path $build 'p0_conpty_host.exe') @('--scenario','product','--child',(Join-Path $build 'piero.exe'),'--results',(Join-Path $repo 'p0-results'),'--label',($Toolchain+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))) 'product-measurement.log'
  # Environment helper's BLOCKED skeleton is kept as helper output, never
  # relabelled a human terminal test. The authoritative run below stays BLOCKED.
  Invoke-Logged (Join-Path $build 'p0_env.exe') @('--dir',(Join-Path $artifacts 'environment-helper'),'--test','FOUNDATION','--run-id',$RunId,'--target',$build,'--fs','NTFS','--notes','Automated clean build and own ConPTY only; not a manual Windows Terminal measurement.') 'environment.log'
  Copy-Item -LiteralPath (Join-Path $artifacts 'environment-helper/environment.json') -Destination (Join-Path $evidence 'environment.json')
  $after = @(Source-Manifest)
  if ($before.Count -ne $after.Count -or (Compare-Object ($before | ConvertTo-Json -Compress) ($after | ConvertTo-Json -Compress))) { throw 'Source changed during validation' }
  Copy-DevelopmentRegressions $repo $artifacts
  $subcaseSkips = @(Select-String -LiteralPath (Join-Path $artifacts 'ctest-detail.log') -Pattern 'SKIP') | ForEach-Object { $_.Line }
  $cases | ForEach-Object { [pscustomobject]@{name=$_.name;result=$(if($_.skipped -or $_.status -eq 'notrun'){'SKIP'}elseif($_.failure){'FAIL'}else{'PASS'});seconds=$_.time} } |
    Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath (Join-Path $evidence 'cases.csv')
  [ordered]@{run_id=$RunId;toolchain=$Toolchain;configuration=$config;compiler=$compiler;build=$build;ninja=$ninja;
    clean_build=$true;product_only_build='PASS: BUILD_TESTING=OFF; no P0/test executable';warning_policy='/W4 /WX';warnings=0;errors=0;ctest_total=$cases.Count;ctest_passed=($cases.Count-$failed-$skipped);ctest_failed=$failed;ctest_skipped=$skipped;
    baseline_preserved=$baseline.Count;subcase_skip_messages=@($subcaseSkips);mutations=$mutationGroups;
    product_sha256=(Get-WfFileSha256 (Join-Path $build 'piero.exe'));
    commits_created=0;production_copy_stop='RETAINED'} |
    ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'validation.json')
  [ordered]@{test_id='FOUNDATION';run_id=$RunId;timestamp_utc=[DateTime]::UtcNow.ToString('o');result='BLOCKED';build_and_regression='PASS';
    notes='Integrated development build measured on this local environment only; release gates remain unresolved. See validation.json, per-P0 runs and STATUS.md.';
    not_tested=@('Windows 10','ReFS','exFAT','SMB/cloud provider','Manual Windows Terminal/conhost/IME matrix (also the editor)','300 real VM power-loss trials','Symbolic-link refusal by the editor/Open (needs link rights)');
    blocked=@('S14 power-loss trials (0 of 300)','S15 owner-absent crash receipt','Image gates A06-A10','Production Copy, Recycle and cross-volume Move gates');
    enabled_by_owner_decision=@('Create/Rename','Same-volume Move','Open','Directory preview','Built-in editor (DR-DRAFT-008)');
    hypotheses='OPEN; no final H-xx determination';production_copy_stop='RETAINED'} |
    ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'run.json')
  Write-Output "run_id=$RunId; CTest=$($cases.Count-$failed-$skipped) PASS/$failed FAIL/$skipped SKIP; mutation kills=$(($mutationGroups | Measure-Object killed -Sum).Sum); RELEASE BLOCKED"
} catch {
  [ordered]@{run_id=$RunId;timestamp_utc=[DateTime]::UtcNow.ToString('o');result='FAIL';reason=$_.Exception.Message;build=$build;production_copy_stop='RETAINED'} |
    ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'run.failure.json')
  throw
} finally { Pop-Location }
