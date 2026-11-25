# Fixed Git argument strings only; output is decoded independently of the console.
function Read-WfGit([string]$Repository, [string]$Arguments) {
  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = (Get-Command git.exe -CommandType Application -ErrorAction Stop).Source
  $start.Arguments = $Arguments
  $start.WorkingDirectory = $Repository
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $start.StandardOutputEncoding = New-Object Text.UTF8Encoding($false, $true)
  $process = New-Object Diagnostics.Process
  $process.StartInfo = $start
  try {
    if (!$process.Start()) { throw 'Cannot start Git' }
    $diagnostic = $process.StandardError.ReadToEndAsync()
    $text = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    $null = $diagnostic.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Git failed: $Arguments" }
    return $text
  } finally { $process.Dispose() }
}
