# Fixed Git argument strings only; output is decoded independently of the console.
function Get-WfGitExecutable {
  # Search absolute PATH entries in order. Empty/relative entries must not
  # select an executable from the repository or the current directory.
  foreach ($entry in ($env:PATH -split ';')) {
    $directory = [Environment]::ExpandEnvironmentVariables($entry.Trim().Trim('"'))
    if ($directory -notmatch '^(?:[A-Za-z]:[\\/]|[\\/]{2}[^\\/]+[\\/][^\\/]+)') { continue }
    $candidate = [IO.Path]::Combine($directory, 'git.exe')
    if ([IO.File]::Exists($candidate)) {
      return [IO.Path]::GetFullPath($candidate)
    }
  }
  throw 'Git executable was not found on absolute PATH entries'
}

function Read-WfGit([string]$Repository, [string]$Arguments) {
  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = Get-WfGitExecutable
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
