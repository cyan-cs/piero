# Runs one P0 collector and files its console output next to its evidence.
#
#   run_p0.ps1 -Exe C:\wffc\p0_copy_callback.exe -Test P0-S01 -- <collector args>
#
# The collector writes <results>\<test>\<run_id>\ itself (run.json,
# environment.json, traces\, artifacts\ ...). This wrapper adds what a C
# program cannot easily know: the git commit (exported as P0_GIT_COMMIT so
# run.json records it) and the captured stdout.log / stderr.log.
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Test,
  [Parameter(Mandatory)][string]$Results,
  [Parameter(ValueFromRemainingArguments)][string[]]$CollectorArgs
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$commit = (git -C $repo rev-parse HEAD 2>$null)
if (-not $commit) { $commit = 'unknown' }
$dirty = (git -C $repo status --porcelain 2>$null)
$env:P0_GIT_COMMIT = if ($dirty) { "$commit+dirty" } else { $commit }

New-Item -ItemType Directory -Force $Results | Out-Null
$before = @(Get-ChildItem (Join-Path $Results $Test) -Directory -ErrorAction SilentlyContinue | % Name)

$out = New-TemporaryFile; $err = New-TemporaryFile
$p = Start-Process -FilePath $Exe -ArgumentList (@('--results', $Results) + $CollectorArgs) `
     -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err

# the run directory is the one that did not exist before
$run = Get-ChildItem (Join-Path $Results $Test) -Directory |
       ? { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($run) {
  Copy-Item $out (Join-Path $run.FullName 'stdout.log')
  Copy-Item $err (Join-Path $run.FullName 'stderr.log')
  # file the Windows event logs that matter for this kind of test
  "exit_code=$($p.ExitCode)" | Out-File (Join-Path $run.FullName 'artifacts\exit_code.txt') -Encoding ascii
  Write-Host "evidence: $($run.FullName)"
}
Get-Content $out
Remove-Item $out, $err -Force
exit $p.ExitCode
