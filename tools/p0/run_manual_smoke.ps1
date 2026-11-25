# Prepares, launches and finalizes a MANUAL terminal smoke run (P0-T01..T08).
#
#   run_manual_smoke.ps1 -TerminalHost wt -Shell pwsh        # prepare + launch sessions
#   run_manual_smoke.ps1 -Finalize <evidence dir>            # after filling result.json
#
# Nothing is simulated. The script opens real terminal windows; a person
# types, looks, and records what happened in result.json. Until then run.json
# stays BLOCKED, and any item left NOT_TESTED keeps the run from becoming PASS.
param(
  [ValidateSet('wt', 'conhost')][string]$TerminalHost = 'wt',
  [ValidateSet('pwsh', 'powershell', 'cmd')][string]$Shell = 'pwsh',
  [string]$Build = 'out/dev',
  [string]$Results = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'p0-results'),
  [string]$Finalize
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if ($Finalize) {
  $dir = (Resolve-Path $Finalize).Path
  $res = Get-Content (Join-Path $dir 'result.json') -Raw | ConvertFrom-Json
  $allowed = 'PASS', 'FAIL', 'NOT_TESTED'
  $bad = $res.items | ? { $allowed -notcontains $_.result }
  if ($bad) { throw "invalid result values: $($bad.id -join ', ')" }
  if (-not $res.observer) { throw "result.json: 'observer' is empty" }
  $fail = @($res.items | ? result -eq 'FAIL'); $nt = @($res.items | ? result -eq 'NOT_TESTED')
  $pass = @($res.items | ? result -eq 'PASS')
  $overall = if ($fail) { 'FAIL' } elseif ($nt) { 'BLOCKED' } else { 'PASS' }
  $run = Get-Content (Join-Path $dir 'run.json') -Raw | ConvertFrom-Json
  $run.result = $overall
  $run.notes = "manual run by '$($res.observer)': $($pass.Count) PASS, $($fail.Count) FAIL, $($nt.Count) NOT_TESTED" +
               $(if ($nt) { " (not tested: $($nt.id -join ', '))" } else { '' })
  $run | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dir 'run.json') -Encoding utf8
  Write-Host "run.json result = $overall ($($run.notes))"
  return
}

$Build = (Resolve-Path -LiteralPath $Build).Path
foreach ($name in 'p0_terminal.exe', 'p0_env.exe') {
  if (!(Test-Path -LiteralPath (Join-Path $Build $name) -PathType Leaf)) {
    throw "Build the terminal collectors first: $name"
  }
}

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$runId = "$stamp-manual-$TerminalHost-$Shell"
$dir = Join-Path $Results "P0-T01\$runId"
foreach ($s in 'traces', 'artifacts', 'eventlog', 'screenshots') { New-Item -ItemType Directory -Force (Join-Path $dir $s) | Out-Null }

$commit = (git -C $repo rev-parse HEAD 2>$null); if (-not $commit) { $commit = 'unknown' }
$env:P0_GIT_COMMIT = if (git -C $repo status --porcelain 2>$null) { "$commit+dirty" } else { $commit }

& (Join-Path $Build 'p0_env.exe') --dir $dir --test P0-T01 --run-id $runId --fs n/a `
   --hyp 'H-023,H-024,H-025,H-026,H-027' --decisions 'D-023,D-025' `
   --notes "manual run prepared; NOT yet performed by a person"
Copy-Item (Join-Path $PSScriptRoot 'manual_smoke_result.template.json') (Join-Path $dir 'result.json')
Copy-Item (Join-Path $repo 'docs\p0-manual-terminal-smoke.md') (Join-Path $dir 'artifacts\procedure.md')

$wtv = (Get-AppxPackage Microsoft.WindowsTerminal -ErrorAction SilentlyContinue | select -First 1).Version
$shv = switch ($Shell) {
  'pwsh'       { (& pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString()') }
  'powershell' { (& powershell -NoProfile -Command '$PSVersionTable.PSVersion.ToString()') }
  'cmd'        { (& cmd /c ver) -join ' ' }
}
@{ terminal_host = $TerminalHost; windows_terminal_version = $(if ($TerminalHost -eq 'wt') { "$wtv" } else { 'n/a' });
   shell = $Shell; shell_version = "$shv"; launched_from_wt_session = [bool]$env:WT_SESSION } |
  ConvertTo-Json | Set-Content (Join-Path $dir 'artifacts\terminal.json') -Encoding utf8

function Launch-Session([string]$Title, [string]$Exe, [string]$ExeArgs) {
  $inner = "`"$Exe`" $ExeArgs"
  switch ($Shell) {
    'pwsh'       { $cmd = "pwsh.exe -NoLogo -NoProfile -Command `"& $inner`"" }
    'powershell' { $cmd = "powershell.exe -NoLogo -NoProfile -Command `"& $inner`"" }
    'cmd'        { $cmd = "cmd.exe /c `"$inner`"" }
  }
  if ($TerminalHost -eq 'wt') { Start-Process wt.exe -ArgumentList "-w new new-tab --title `"$Title`" $cmd" }
  else                        { Start-Process conhost.exe -ArgumentList $cmd }
}

function Wait-LogClosed([string]$Log, [int]$Minutes = 20) {
  $end = (Get-Date).AddMinutes($Minutes)
  while ((Get-Date) -lt $end) {
    if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern 'TERM_CLOSED' -Quiet -ErrorAction SilentlyContinue)) { return }
    Start-Sleep -Milliseconds 500
  }
  Write-Warning "timed out waiting for $Log"
}

$pt = Join-Path $Build 'p0_terminal.exe'
Write-Host "`nEvidence directory: $dir`n"
Write-Host "SESSION 1/3  key logging (T01/T02): type the keys listed in artifacts\procedure.md section 2, then press Shift+Q."
$l1 = Join-Path $dir 'traces\input_log.csv'
Launch-Session 'P0 input log' $pt "--mode log-input --log `"$l1`" --timeout-ms 900000"; Wait-LogClosed $l1

Write-Host "SESSION 2/3  modal guard (T05/T02): follow procedure section 3. Quit with Ctrl+C."
$l2 = Join-Path $dir 'traces\confirm_log.csv'
Launch-Session 'P0 confirm' $pt "--mode confirm-test --log `"$l2`" --timeout-ms 900000"; Wait-LogClosed $l2

Write-Host "SESSION 3/3  width matrix (T03/T04): look, record, press Q."
$l3 = Join-Path $dir 'traces\width_log.csv'
Launch-Session 'P0 width matrix' $pt "--mode width-matrix --log `"$l3`" --timeout-ms 900000"; Wait-LogClosed $l3

Write-Host "PRODUCT SMOKE: follow procedure section 5 using a separately prepared disposable fixture."
Write-Host "The product is not launched here. Leave M01-M12 NOT_TESTED until you observe them."

Write-Host "`nNow open $dir\result.json, fill in every item you actually performed"
Write-Host "(leave the rest NOT_TESTED), add screenshots to screenshots\, then run:"
Write-Host "  tools\p0\run_manual_smoke.ps1 -Finalize `"$dir`""
