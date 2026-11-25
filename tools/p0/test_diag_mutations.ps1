param(
  [Parameter(Mandatory)][string]$Compiler,
  [Parameter(Mandatory)][string]$Build,
  [ValidateSet('Debug','Release')][string]$Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
Initialize-WfToolchain
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = (Resolve-Path -LiteralPath $Build).Path
$san = Get-WfSanitizer $Compiler $buildRoot
$root = Join-Path $buildRoot ('diag-mutants-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$diagSources = @('wf_diag.c','wf_diag_store.c','wf_diag_validate.c','wf_diag_format.c')
$originals = @{}
foreach ($source in $diagSources) {
  $originals[$source] = [IO.File]::ReadAllText((Join-Path $repo "src/diagnostics/$source"))
}
$mutants = @(
  @{name='event_path_privacy';old='if (log->paths && event->path)';new='if ((log->paths || 1) && event->path)'},
  @{name='severity_filter';old='if (event->level > log->level)';new='if (event->level < log->level)'},
  @{name='rotation_size';old='bytes + sizeof prefix + text.len > WF_DIAG_FILE_BYTES';new='bytes + sizeof prefix + text.len > 2u * WF_DIAG_FILE_BYTES'},
  @{name='edited_body_crc';old='if (!wf_diag_records_valid(bytes, size))';new='if (0 && !wf_diag_records_valid(bytes, size))'},
  @{name='unknown_runtime_object';old='return wf_diag_known_entries(log, false, error);';new='return 1;'},
  @{name='hardlink_refusal';old='1 != standard.NumberOfLinks';new='0 == standard.NumberOfLinks'},
  @{name='record_crc_write';old='prefix.crc32 = wf_diag_crc32(text.data, text.len);';new='prefix.crc32 = 0;'},
  @{name='strict_utf8';old='MB_ERR_INVALID_CHARS';new='0'}
)
$sources = @('tests/test_diag_win.c','src/path/cwd_neutralize.c','src/path/wf_canonical_win.c',
  'src/path/wf_path.c','src/base/wf_buf.c','src/text/wf_sanitize.c','src/text/wf_utf.c','src/text/wf_width.c',
  'src/fs/owned_delete.c','src/fs/owned_common.c','src/fs/owned_payload.c',
  'src/fs/owned_committed.c','src/fs/owned_local.c','src/fs/owned_live.c','src/fs/owned_log.c',
  'tools/p0/p0_lab.c','tools/p0/p0_area.c','tools/p0/p0_common.c')
$results = @()
foreach ($m in (@(@{name='control';control=$true}) + $mutants)) {
  $dir = Join-Path $root $m.name
  New-Item -ItemType Directory -Path $dir | Out-Null
  $locations = 0
  if (!$m.control) {
    foreach ($source in $diagSources) {
      $locations += Measure-WfAnchor $originals[$source] $m.old
    }
  }
  if (!$m.control -and $locations -ne 1) {
    throw "Mutation requires one exact source location: $($m.name)"
  }
  $files = @()
  foreach ($source in $diagSources) {
    $file = Join-Path $dir $source
    $text = if ($m.control) { $originals[$source] } else { Edit-WfAnchor $originals[$source] $m.old $m.new }
    [IO.File]::WriteAllText($file,$text)
    $files += $file
  }
  $exe = Join-Path $dir 'test_diag.exe'
  $runtime = Get-WfRuntime $Configuration $san
  $args = @('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,
    '/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
    "/I$repo/src","/I$repo/src/diagnostics","/I$repo/tests","/Fo$dir/","/Fe$exe")
  $args += $files
  foreach ($source in $sources) { $args += Join-Path $repo $source }
  $args += @('/link','advapi32.lib','bcrypt.lib')
  $args = Add-WfSanitizerArgs $args $san
  & $Compiler @args *> (Join-Path $dir 'build.log')
  if ($LASTEXITCODE -ne 0) { throw "Mutation compile failed (never counted as killed): $($m.name), $dir/build.log" }
  & $exe *> (Join-Path $dir 'test.log')
  if ($m.control) {
    if ($LASTEXITCODE -ne 0 -or
        (Select-String -LiteralPath (Join-Path $dir 'test.log') -Pattern '^FAIL ' -CaseSensitive -Quiet)) {
      throw 'Unmodified diagnostics control failed; no mutant verdict is valid'
    }
    Write-Output 'Unmodified diagnostics control: PASS'
    continue
  }
  $results += Test-WfMutant $m.name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $null)
