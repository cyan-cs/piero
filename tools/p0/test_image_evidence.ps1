param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$WireTest,
  [string]$WireAsan = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$testRoot = Join-Path (Resolve-Path $Build).Path ('image-evidence-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$work = Join-Path $testRoot 'work'; New-Item -ItemType Directory -Path $work | Out-Null
$results = Join-Path $testRoot 'results'
$oldHashes = @{}
foreach ($iteration in 1..2) {
  & (Join-Path $PSScriptRoot 'run_image_evidence.ps1') -Exe $Exe -Results $results -WorkBase $work -WireTest $WireTest -WireAsan $WireAsan
  if ($LASTEXITCODE -notin @(0, 1, 77)) { throw "Unexpected collector exit $LASTEXITCODE" }
  foreach ($test in @('P0-A06','P0-A07','P0-A08','P0-A09','P0-A10')) {
    $runs = @(Get-ChildItem -LiteralPath (Join-Path $results $test) -Directory)
    if ($runs.Count -ne $iteration) { throw "Append-only run count wrong: $test" }
    foreach ($run in $runs) {
      $jsonPath = Join-Path $run.FullName 'run.json'
      $record = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
      if ($record.result -ne 'BLOCKED') { throw "Incomplete phase promoted: $test" }
      foreach ($relative in @('stdout.log','stderr.log','environment.json','traces/profile_creation.txt',
          'artifacts/matrix.csv','artifacts/remaining.txt','artifacts/source-freeze.json','artifacts/candidate-ipc.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $run.FullName $relative) -PathType Leaf)) { throw "Missing $relative in $($run.Name)" }
      }
      $matrix = @(Import-Csv -LiteralPath (Join-Path $run.FullName 'artifacts/matrix.csv'))
      if (@($matrix | Where-Object { $_.environment -eq 'Windows 10 22H2' -and $_.status -eq 'NOT_TESTED' }).Count -ne 1) {
        throw 'Non-current Windows environment was not explicitly marked untested'
      }
      $candidate = Get-Content -LiteralPath (Join-Path $run.FullName 'artifacts/candidate-ipc.json') -Raw | ConvertFrom-Json
      if ($candidate.standalone.exit_code -ne 0 -or $candidate.gate_result -ne 'BLOCKED') { throw 'Candidate parser result conflated with gate' }
      $hash = Get-WfFileSha256 $jsonPath
      if ($oldHashes.ContainsKey($jsonPath) -and $oldHashes[$jsonPath] -ne $hash) { throw 'Prior run was overwritten' }
      $oldHashes[$jsonPath] = $hash
    }
  }
}
Write-Output "ok: append-only image BLOCKED evidence ($testRoot)"
exit 0
