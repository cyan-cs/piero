param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Results)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$run = 'evidence-smoke-' + [guid]::NewGuid().ToString('N')
$root = Join-Path $Results $run
New-Item -ItemType Directory -Path $root | Out-Null
& $Exe --evidence $root $run *> (Join-Path $root 'first.log')
if ($LASTEXITCODE -ne 0) { throw "Evidence collector failed: $LASTEXITCODE; $root/first.log" }
$files = @()
foreach ($id in @('P0-S04','P0-S05','P0-S06')) {
  $dir = Join-Path $root "$id/$run"
  foreach ($name in @('run.json','environment.json','cases.csv')) {
    $path = Join-Path $dir $name
    if (!(Test-Path -LiteralPath $path)) { throw "Missing $path" }
    if ($name.EndsWith('.json')) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json | Out-Null }
    $files += [pscustomobject]@{path=$path;hash=(Get-WfFileSha256 $path)}
  }
}
# A duplicate run ID must refuse before opening any existing evidence for write.
$ErrorActionPreference = 'Continue' # stderr is expected for this refusal.
& $Exe --evidence $root $run *> (Join-Path $root 'duplicate.log')
$duplicateCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($duplicateCode -ne 2) { throw 'Collector did not reject an existing run' }
foreach ($file in $files) {
  if ((Get-WfFileSha256 $file.path) -ne $file.hash) { throw "Evidence overwritten: $($file.path)" }
}
'evidence JSON complete; duplicate invocation refused; hashes unchanged'
