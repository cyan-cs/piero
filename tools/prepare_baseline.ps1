# Produces a reviewable candidate manifest; never stages or commits anything.
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wf_hash.ps1')
. (Join-Path $PSScriptRoot 'wf_git.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
if (Test-Path -LiteralPath $Output) { throw 'Refusing to overwrite a baseline manifest' }
$previous = Get-Location
try {
  Set-Location -LiteralPath $repo
  $head = (Read-WfGit $repo 'rev-parse --verify HEAD').Trim()
  # NUL-delimited UTF-8 bypasses Git quoting and the console's legacy code page.
  $listed = Read-WfGit $repo 'ls-files -z --cached --others --exclude-standard'
  $paths = @($listed.Split([char[]]@([char]0), [StringSplitOptions]::RemoveEmptyEntries) |
    Sort-Object -Unique -CaseSensitive)
  $findings = @()
  $files = foreach ($path in $paths) {
    $file = Get-Item -LiteralPath (Join-Path $repo $path)
    if ($file.Length -gt 1048576) { $findings += "Large file: $path" }
    if ($path -match '(?i)(^|/)(\.env[^/]*|credentials|id_rsa|id_ed25519)$|\.(pfx|p12|pem|key|exe|dll|obj|lib|pdb)$') {
      $findings += "Excluded material in candidates: $path"
    }
    # Report locations only, never print a matched secret value.
    $suspicious = @(Select-String -LiteralPath $path -Pattern '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----','\bAKIA[0-9A-Z]{16}\b','\bgh[pousr]_[A-Za-z0-9]{30,}\b')
    foreach ($hit in $suspicious) { $findings += "Possible secret: $path line $($hit.LineNumber)" }
    [pscustomobject]@{path=$path;bytes=$file.Length;sha256=(Get-WfFileSha256 $file.FullName)}
  }
  $manifest = [ordered]@{created_utc=[DateTime]::UtcNow.ToString('o');git_head=$head;
    action='REVIEW_ONLY: no index changes, no commit';secret_scan='pattern scan only; not a guarantee against every secret';
    result=$(if($findings.Count){'BLOCKED'}else{'READY_FOR_REVIEW'});findings=$findings;files=@($files)} |
    ConvertTo-Json -Depth 6
  $stream = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
  $writer = $null
  try {
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($true))
    $writer.WriteLine($manifest)
  } finally {
    if ($writer) { $writer.Dispose() } else { $stream.Dispose() }
  }
  if ($findings.Count) { throw ($findings -join "`n") }
  Write-Output "Prepared $($files.Count) Git candidates; index unchanged: $Output"
} finally { Set-Location -LiteralPath $previous }
