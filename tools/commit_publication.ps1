# Run from the original checkout in a normal authorized developer PowerShell.
# Validates a frozen handoff; never applies its patch, changes ACLs or pushes.
param([string]$Manifest = 'out/publication-audit/final-handoff/manifest.json', [switch]$Commit)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $repo
. (Join-Path $PSScriptRoot 'wf_git.ps1')
$gitExe = Get-WfGitExecutable
$plan = Get-Content -Raw -Encoding UTF8 -LiteralPath $Manifest | ConvertFrom-Json
$head = & $gitExe rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot read HEAD' }
if ($head -ne $plan.base) { throw 'HEAD changed; regenerate/review the handoff' }
& $gitExe diff --cached --quiet
if ($LASTEXITCODE) { throw 'Index is not empty or unreadable; nothing staged' }
$patch = Join-Path (Split-Path -Parent $Manifest) $plan.patch
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash -ne $plan.patch_sha256) { throw 'Patch hash mismatch' }
function Resolve-PublicationPath([string]$Relative) {
  if (!$Relative -or $Relative -match '[\\:\x00-\x1f*?"<>|]') { throw 'Unsafe manifest path' }
  $parts = $Relative.Split('/')
  $path = $repo
  foreach ($part in $parts) {
    if (!$part -or $part -in @('.', '..') -or $part.EndsWith('.') -or $part.EndsWith(' ')) { throw 'Noncanonical manifest path' }
    $path = Join-Path $path $part
    if (Test-Path -LiteralPath $path) {
      if ((Get-Item -Force -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in manifest path' }
    }
  }
  return $path
}
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $plan.files) {
  $path = Resolve-PublicationPath $entry.path
  if (!$seen.Add($entry.path)) { throw 'Duplicate manifest path' }
  if ($entry.path -eq '.mcp.json' -or $entry.path -match '^(\.superpowers|\.codex|\.agents)(/|$)') { throw 'Local configuration in stage list' }
  if ($entry.sha256) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $entry.sha256) { throw "Worktree changed: $($entry.path)" }
  } elseif (Test-Path -LiteralPath $path) { throw "Expected deletion changed: $($entry.path)" }
}
foreach ($path in $plan.untrack_keep_local) {
  $null = Resolve-PublicationPath $path
  if (!$seen.Add($path)) { throw 'Duplicate manifest path' }
  if ($path -ne '.mcp.json' -and $path -notmatch '^\.superpowers/') { throw 'Unexpected untrack path' }
}
foreach ($entry in $plan.files) {
  & $gitExe --literal-pathspecs add -- $entry.path
  if ($LASTEXITCODE) { throw 'Staging denied/failed; preserve worktree, do not change ACLs' }
}
foreach ($path in $plan.untrack_keep_local) {
  & $gitExe --literal-pathspecs rm --cached -- $path
  if ($LASTEXITCODE) { throw 'Untracking failed; local files are preserved' }
}
& $gitExe diff --cached --check
if ($LASTEXITCODE) { throw 'Staged whitespace/error check failed' }
& $gitExe diff --cached --stat
if ($Commit) {
  & $gitExe commit -m $plan.message
  if ($LASTEXITCODE) { throw 'Commit failed; preserve index and worktree' }
}
& $gitExe status --short
