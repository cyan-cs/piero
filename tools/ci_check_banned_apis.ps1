# CI-DEL-01 / CI-PATH-01 / CI-NOCLOBBER-01 / CI-CANCEL-01 / CI-OWNEDKIND-01
# Usage: ci_check_banned_apis.ps1 <src-root>
param([Parameter(Mandatory)][string]$Root)

# WSL has no file identity, so its path-based removal boundary is allowlisted
# alongside owned_delete (DR-DRAFT-014, 2b).
$ownedDelete = @('src/fs/owned_delete.c', 'src/fs/owned_delete.h',
  'src/fs/wf_wslrm_win.c', 'src/fs/wf_wslrm_win.h')
$startupCwd  = @('src/path/cwd_neutralize.c')

# pattern, id, files that may legitimately contain it
$rules = @(
  @{ Re = '\b(DeleteFile[AW]?|RemoveDirectory[AW]?|FILE_FLAG_DELETE_ON_CLOSE|FileDispositionInfo(Ex)?|FILE_DISPOSITION_)\b'
     Id = 'CI-DEL-01'; Allow = $ownedDelete },
  @{ Re = '\bSetCurrentDirectory[AW]?\b'
     Id = 'CI-PATH-01'; Allow = $startupCwd },
  @{ Re = '\b(MOVEFILE_REPLACE_EXISTING|FILE_RENAME_FLAG_REPLACE_IF_EXISTS)\b'
     Id = 'CI-NOCLOBBER-01'; Allow = @() },
  @{ Re = '\bPROGRESS_CANCEL\b'
     Id = 'CI-CANCEL-01'; Allow = @() },
  @{ Re = '\bWF_OWNED_(MISC|STATE|ANY|GENERIC|OTHER|UNKNOWN)\b'
     Id = 'CI-OWNEDKIND-01'; Allow = @() }
)

$repo = Split-Path -Parent (Resolve-Path $Root)
$bad = 0
# src/third_party is other people's code; what of it is compiled is checked below
Get-ChildItem -Path $Root -Recurse -Include *.c, *.h | Where-Object {
  -not $_.FullName.Replace('\', '/').Contains('/src/third_party/') } | ForEach-Object {
  $rel = $_.FullName.Substring($repo.Length + 1).Replace('\', '/')
  # @() matters: a one-line file would otherwise be a string, not an array
  $lines = @(Get-Content -LiteralPath $_.FullName)
  foreach ($r in $rules) {
    if ($r.Allow -contains $rel) { continue }
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match $r.Re) {
        "{0}: {1}:{2}: {3}" -f $r.Id, $rel, ($i + 1), $lines[$i].Trim()
        $bad++
      }
    }
  }
}
# CI-ARCHIVE-RO-01: libarchive reads through callbacks; application sinks own
# extraction writes. Forbid libarchive writers and direct extraction-to-disk
# APIs, and exclude their sources from the libarchive build.
$roRule = 'archive_(write|read_extract|read_disk|read_open_filename|read_data_into_fd)'
Get-ChildItem -Path (Join-Path $Root 'archive') -Include *.c, *.h -Recurse | ForEach-Object {
  $rel = $_.FullName.Substring($repo.Length + 1).Replace('\', '/')
  $n = 0
  foreach ($l in Get-Content -LiteralPath $_.FullName) {
    $n++
    if ($l -match $roRule) { "CI-ARCHIVE-RO-01: {0}:{1}: {2}" -f $rel, $n, $l.Trim(); $bad++ }
  }
}
$tp = Join-Path $Root 'third_party/CMakeLists.txt'
$n = 0
foreach ($l in Get-Content -LiteralPath $tp) {
  $n++
  if ($l -match 'archive_(write|read_extract|read_disk|read_open_filename|read_data_into_fd)') {
    "CI-ARCHIVE-RO-01: src/third_party/CMakeLists.txt:{0}: {1}" -f $n, $l.Trim(); $bad++
  }
}
if ($bad) { exit 1 }
"banned API check: clean"
