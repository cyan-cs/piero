param(
  [Parameter(Mandatory)][string]$Build,
  [string]$RunId,
  [ValidateRange(1,1000)][int]$LiveIterations = 10,
  [string]$Results
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = (Resolve-Path -LiteralPath $Build).Path
if (!$Results) { $Results = Join-Path $repo 'p0-results' }
if (!$RunId) { $RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-recovery-' + [guid]::NewGuid().ToString('N').Substring(0,8) }
if ($RunId -notmatch '^[A-Za-z0-9_-]{1,96}$') { throw 'Unsafe evidence run ID' }
$ids = @('P0-S11','P0-S12','P0-S13','P0-S15','P0-S16')
foreach ($id in $ids) { if (Test-Path -LiteralPath (Join-Path $Results "$id/$RunId")) { throw "Refusing existing evidence: $id/$RunId" } }
$recovery = Join-Path $buildRoot 'test_recovery_win.exe'
if (!(Test-Path -LiteralPath $recovery)) { $recovery = Join-Path $buildRoot 'test_recovery.exe' }
$store = Join-Path $buildRoot 'test_recovery_store.exe'
$partial = Join-Path $buildRoot 'test_recovery_partial.exe'
$suite = Join-Path $buildRoot 'p0_recovery_suite.exe'
foreach ($exe in @($recovery,$store,$partial,$suite)) { if (!(Test-Path -LiteralPath $exe)) { throw "Missing measurement executable: $exe" } }
$outsideLog = Join-Path $buildRoot ("recovery-process-$RunId.log")
if (Test-Path -LiteralPath $outsideLog) { throw 'Refusing existing measurement log' }
& $suite $Results $RunId *> $outsideLog
$suiteExit = $LASTEXITCODE
$environment = Get-Content -LiteralPath (Join-Path $Results "P0-S13/$RunId/environment.json") -Raw | ConvertFrom-Json
$overallFailure = $suiteExit -ne 0
foreach ($id in @('P0-S11','P0-S15','P0-S16')) {
  $dir = Join-Path $Results "$id/$RunId"
  $artifacts = Join-Path $dir 'artifacts'
  New-Item -ItemType Directory -Path $artifacts | Out-Null
  $exe = if ($id -eq 'P0-S16') { $store } else { $recovery }
  $arguments = if ($id -eq 'P0-S11') { @('--live') } elseif ($id -eq 'P0-S15') { @('--residue') } else { @() }
  $rows = @()
  $iterations = if ($id -eq 'P0-S11') { $LiveIterations } else { 1 }
  for ($i = 1; $i -le $iterations; $i++) {
    & $exe @arguments *> (Join-Path $artifacts "measurement-$i.log")
    $code = $LASTEXITCODE
    $rows += [pscustomobject]@{test_id=$id; iteration=$i; suite=$(if($arguments.Count){$arguments[0]}else{'local_abandon'}); exit_code=$code; result=$(if($code -eq 0){'PASS'}else{'FAIL'})}
  }
  if($id -eq 'P0-S16'){
    & $partial *> (Join-Path $artifacts 'partial-state-faults.log')
    $partialExit=$LASTEXITCODE
    $rows += [pscustomobject]@{test_id=$id;iteration=1;suite='partial_states_fault_injection';exit_code=$partialExit;result=$(if($partialExit -eq 0){'PASS'}else{'FAIL'})}
  }
  $rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir 'cases.csv')
  $pass = @($rows | Where-Object result -ne 'PASS').Count -eq 0
  if (!$pass) { $overallFailure = $true }
  $environment | Add-Member -NotePropertyName measurement_executable -NotePropertyValue (Split-Path -Leaf $exe) -Force
  $environment | Add-Member -NotePropertyName measurement_sha256 -NotePropertyValue (Get-WfFileSha256 $exe) -Force
  $environment | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $dir 'environment.json')
  [ordered]@{test_id=$id;run_id=$RunId;timestamp_utc=[DateTime]::UtcNow.ToString('o');result=$(if(!$pass){'FAIL'}elseif($id -eq 'P0-S15'){'BLOCKED'}else{'PASS'});measured_cases=$(if($pass){'PASS'}else{'FAIL'});build=$buildRoot;
    notes=$(if($id -eq 'P0-S11'){'Live-only atomic CREATE_NEW manifest; durable journal callback; reverse handle cleanup; unrelated user file survives and prevents nonempty parent removal; process restart cannot adopt a root.'}elseif($id -eq 'P0-S15'){'Exact committed destination identity/size; owner nonce and empty txn cleanup via retained same-token receipt; adversarial cases preserved. BLOCKED for reconstructed empty crash residue after owner disappearance: no durable nonce receipt record, safe MANUAL.'}else{'Exact typed recovery ID; local kind-specific journal/meta ownership; durable abandon intent before disposition and completion afterward; unavailable SMB/removed-volume strings traced at Win32 boundary with zero target I/O; second record retained.'});
    matrix=[ordered]@{tested='Current local Windows/NTFS only; see environment.json';windows10='NOT_TESTED';ReFS='NOT_TESTED';SMB_target='NOT_TESTED (metadata-only no-I/O abandonment traced)';powerloss='NOT_TESTED; process termination is not power-loss evidence';production_copy_gate='RETAINED'}
  } | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $dir 'run.json')
}
$sources = @('src/fs/owned_delete.c','src/fs/owned_delete.h',
    'src/fs/owned_common.c','src/fs/owned_payload.c','src/fs/owned_committed.c','src/fs/owned_local.c','src/fs/owned_live.c','src/fs/owned_log.c','src/fs/owned_internal.h',
    'tools/p0/owned_test_sources.ps1','tests/wf_owned_fault_shim.h','tests/wf_recovery_fault_shim.h','src/recovery/wf_recovery.c','src/recovery/wf_recovery.h',
  'src/recovery/wf_recovery_store.c','src/recovery/wf_recovery_store.h','tools/p0/p0_journal.c','tools/p0/p0_journal.h',
  'tools/p0/p0_copy_op.c','tools/p0/p0_recovery_suite.c','tools/p0/run_recovery.ps1',
  'tests/test_recovery_win.c','tests/test_recovery_store.c','tests/test_recovery_partial.c','tests/test_recovery_journal.c',
  'src/diagnostics/wf_diag_format.h','tests/test_recovery_log.c','tools/p0/test_log_mutations.ps1',
  'tools/p0/test_storage_mutations.ps1','tools/p0/test_recovery_mutations.ps1')
$manifest = $sources | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-WfFileSha256 (Join-Path $repo $_))} }
foreach ($id in $ids) {
  $artifacts = Join-Path $Results "$id/$RunId/artifacts"
  if (!(Test-Path -LiteralPath $artifacts)) { New-Item -ItemType Directory -Path $artifacts | Out-Null }
  $manifest | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $artifacts 'source-manifest.json')
  Copy-Item -LiteralPath $outsideLog -Destination (Join-Path $artifacts 'process-suite.log')
}
"run_id=$RunId; S12/S13 exit=$suiteExit; S11 iterations=$LiveIterations; overall=$(if($overallFailure){'FAIL'}else{'MEASURED_PASS_S15_GATE_BLOCKED'})"
if ($overallFailure) { exit 1 }
