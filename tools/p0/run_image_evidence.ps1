param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Results,
  [Parameter(Mandatory)][string]$WorkBase,
  [Parameter(Mandatory)][string]$WireTest,
  [string]$WireAsan = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Exe = (Resolve-Path $Exe).Path
$WireTest = (Resolve-Path $WireTest).Path
$WorkBase = (Resolve-Path $WorkBase).Path
New-Item -ItemType Directory -Force -Path $Results | Out-Null
$Results = (Resolve-Path $Results).Path
$capture = Join-Path $Results ('image-capture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $capture | Out-Null
$before = @{}
foreach ($phase in @('P0-A06','P0-A07','P0-A08','P0-A09','P0-A10')) {
  $phaseRoot = Join-Path $Results $phase
  if (Test-Path -LiteralPath $phaseRoot) {
    foreach ($run in Get-ChildItem -LiteralPath $phaseRoot -Directory) { $before[$run.FullName] = $true }
  }
}
$freeze = @('src/preview/wf_preview.c','src/preview/wf_preview.h',
  'src/preview/wf_image_wire.c','src/preview/wf_image_wire.h') | ForEach-Object {
  [pscustomobject]@{source=$_;sha256=(Get-WfFileSha256 (Join-Path $repo $_))}
}
$stdout = Join-Path $capture 'stdout.log'; $stderr = Join-Path $capture 'stderr.log'
& $Exe --results $Results --work-base $WorkBase 1> $stdout 2> $stderr
$collectorExit = $LASTEXITCODE

$wireOutput = Join-Path $capture 'candidate-standalone.log'
& $WireTest *> $wireOutput
$wireExit = $LASTEXITCODE
$candidate = [ordered]@{
  gate_result='BLOCKED'
  reason='Standalone parser checks do not qualify sandbox raster decoding, recycling or the complete instrumented product. Image qualification remains unresolved; A03 prior FAIL unchanged.'
  standalone=[ordered]@{exe_sha256=(Get-WfFileSha256 $WireTest);exit_code=$wireExit}
  instrumented=[ordered]@{result='NOT_RUN';note='ASan binary not supplied'}
  source_hash_note='Frozen source identities; this is not a reproducible-build attestation.'
}
$asanExit = 0
if ($WireAsan) {
  $WireAsan = (Resolve-Path $WireAsan).Path
  & $WireAsan *> (Join-Path $capture 'candidate-asan.log')
  $asanExit = $LASTEXITCODE
  $candidate.instrumented = [ordered]@{result=$(if($asanExit -eq 0){'PASS'}else{'FAIL'});exe_sha256=(Get-WfFileSha256 $WireAsan);exit_code=$asanExit}
}
foreach ($entry in $freeze) {
  if ((Get-WfFileSha256 (Join-Path $repo $entry.source)) -ne $entry.sha256) {
    throw "Source changed during capture: $($entry.source)"
  }
}
$newRuns = @()
foreach ($phase in @('P0-A06','P0-A07','P0-A08','P0-A09','P0-A10')) {
  $phaseRoot = Join-Path $Results $phase
  $new = @(Get-ChildItem -LiteralPath $phaseRoot -Directory | Where-Object { !$before.ContainsKey($_.FullName) })
  if ($new.Count -ne 1) { throw "Collector did not write exactly one new $phase record (exit $collectorExit)" }
  $run = $new[0]; $newRuns += $run.FullName
  $record = Get-Content -LiteralPath (Join-Path $run.FullName 'run.json') -Raw | ConvertFrom-Json
  if ($record.result -ne 'BLOCKED') { throw "Incomplete image phase unexpectedly promoted: $phase" }
  Copy-Item -LiteralPath $stdout -Destination (Join-Path $run.FullName 'stdout.log')
  Copy-Item -LiteralPath $stderr -Destination (Join-Path $run.FullName 'stderr.log')
  Copy-Item -LiteralPath $wireOutput -Destination (Join-Path $run.FullName 'traces/candidate-standalone.log')
  if ($WireAsan) { Copy-Item -LiteralPath (Join-Path $capture 'candidate-asan.log') -Destination (Join-Path $run.FullName 'traces/candidate-asan.log') }
  $freeze | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run.FullName 'artifacts/source-freeze.json') -Encoding utf8
  $candidate | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run.FullName 'artifacts/candidate-ipc.json') -Encoding utf8
  "collector_exit=$collectorExit" | Set-Content -LiteralPath (Join-Path $run.FullName 'artifacts/exit_code.txt') -Encoding ascii
}
$newRuns | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $capture 'runs.json') -Encoding utf8
Get-Content -LiteralPath $stdout
if ($wireExit -ne 0 -or $asanExit -ne 0) { exit 1 }
exit $collectorExit
