# Size, start-up time, idle memory and idle CPU of one piero.exe.
#
#   powershell -File tools/measure_exe.ps1 -Exe out\rel\piero.exe [-Runs 40]
#
# Start-up is the wall time of "piero --version" (process creation, loader and
# C runtime, argument handling, exit), median and p95 over -Runs starts. Idle
# memory and CPU are taken from the real program running on a hidden console in
# an empty folder for 4 seconds. The process started here is the only one
# touched.
param([Parameter(Mandatory)][string]$Exe, [int]$Runs = 40)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$size = (Get-Item $Exe).Length

$times = @()
for ($i = 0; $i -lt $Runs + 3; $i++) {
  $psi0 = New-Object Diagnostics.ProcessStartInfo
  $psi0.FileName = $Exe
  $psi0.Arguments = '--version'
  $psi0.UseShellExecute = $false
  $psi0.CreateNoWindow = $true
  $psi0.RedirectStandardOutput = $true
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = [Diagnostics.Process]::Start($psi0)
  [void]$p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  $sw.Stop()
  if ($i -ge 3) { $times += $sw.Elapsed.TotalMilliseconds }
}
$sorted = $times | Sort-Object
$median = $sorted[[int]($sorted.Count / 2)]
$p95 = $sorted[[int]($sorted.Count * 0.95)]

$dir = Join-Path ([IO.Path]::GetTempPath()) ('wf_measure_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir | Out-Null
$cfg = Join-Path $dir 'c.ini'
Set-Content $cfg "[general]`nlanguage = en`n[appearance]`ntheme = dark`n" -Encoding ascii
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "--language en --config `"$cfg`" `"$dir`""
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$proc = [Diagnostics.Process]::Start($psi)
Start-Sleep -Milliseconds 1500
$proc.Refresh()
$cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
Start-Sleep -Milliseconds 4000
$proc.Refresh()
$cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
$ws = $proc.WorkingSet64 / 1KB
$priv = $proc.PrivateMemorySize64 / 1KB
$alive = -not $proc.HasExited
if ($alive) { $proc.Kill() }
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue

'{0}' -f $Exe
'  size                 {0,10:N0} bytes' -f $size
'  start (--version)    median {0,6:N1} ms   p95 {1,6:N1} ms   ({2} runs)' -f $median, $p95, $Runs
'  idle working set     {0,10:N0} KiB   private {1,10:N0} KiB   (running: {2})' -f $ws, $priv, $alive
'  idle CPU             {0,6:N1} ms over 4 s ({1:N2} %)' -f ($cpu1 - $cpu0), (($cpu1 - $cpu0) / 4000 * 100)
