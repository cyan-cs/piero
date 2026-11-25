param(
  [Parameter(Mandatory)][string]$Compiler,
  [Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$Configuration
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
Initialize-WfToolchain
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = (Resolve-Path $Build).Path
$san = Get-WfSanitizer $Compiler $buildRoot
$root = Join-Path $buildRoot ('preflight-mutants-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$sources = @{
  guard=[IO.File]::ReadAllText((Join-Path $repo 'tools/p0/p0_source_guard.c'))
  preflight=[IO.File]::ReadAllText((Join-Path $repo 'src/xfer/wf_preflight.c'))
  preflight_walk=[IO.File]::ReadAllText((Join-Path $repo 'src/xfer/wf_preflight_walk.c'))
  preflight_paths=[IO.File]::ReadAllText((Join-Path $repo 'src/xfer/wf_preflight_paths.c'))
  paths=[IO.File]::ReadAllText((Join-Path $repo 'src/xfer/wf_xfer_paths.c'))
}
$mutants = @(
  @{Name='guard_file_id';File='guard';Old='memcmp(id.FileId.Identifier, snapshot->id.id, 16)';New='0';Test='guard'},
  @{Name='guard_volume';File='guard';Old='id.VolumeSerialNumber != snapshot->id.volume';New='0';Test='guard'},
  @{Name='guard_size';File='guard';Old='(uint64_t)standard.EndOfFile.QuadPart != snapshot->size';New='0';Test='guard'},
  @{Name='guard_time';File='guard';Old='(uint64_t)basic.LastWriteTime.QuadPart != snapshot->mtime';New='0';Test='guard'},
  @{Name='guard_share';File='guard';Old='FILE_SHARE_READ, NULL, OPEN_EXISTING';New='FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING';Test='guard'},
  @{Name='guard_release';File='guard';Old='CloseHandle(guard->handle);';New='(void)guard->handle;';Test='guard'},
  @{Name='guard_busy_reason';File='guard';Old='P0GuardResult result = P0_SOURCE_BUSY;';New='P0GuardResult result = P0_SOURCE_CHANGED;';Test='poc'},
  @{Name='batch_completion';File='paths';Old='report->checks_complete &&';New='1 &&';Test='unit'},
  @{Name='cloud_consent';File='paths';Old='(!report->cloud_files || 1 == hydration_allowed)';New='(hydration_allowed || 1)';Test='unit'},
  @{Name='transaction_component';File='paths';Old='WF_TXN_NAME_UNITS';New='0';Test='unit'},
  @{Name='path_integer_bound';File='paths';Old='dest_units > SIZE_MAX - fixed';New='0';Test='unit'},
  @{Name='snapshot_identity';File='preflight';Old='(snap->id.kind == WF_ID_STRONG) && !wf_fileid_equal(&snap->id, &now->id)';New='0';Test='unit'},
  @{Name='snapshot_cloud_state';File='preflight';Old='const uint32_t kind = WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD;';New='const uint32_t kind = WF_ENT_DIR | WF_ENT_LINK;';Test='unit'},
  @{Name='batch_collision';File='preflight_paths';Old="if (WF_OK == st) { return wf_pf_verdict(rep, WF_PF_COLLISION, i, name); }";New='if (WF_OK == st) { st = WF_E_NOENT; }';Test='unit'},
  @{Name='unknown_collision';File='preflight_paths';Old='if (WF_E_NOENT != st) { /* unknown is not';New='if (WF_E_NOENT != st && WF_E_OS != st) { /* unknown is not';Test='unit'},
  @{Name='reserve_overflow';File='preflight';Old='rep->total_bytes > UINT64_MAX - pol->reserve_bytes';New='0';Test='unit'},
  @{Name='final_path_limit';Files=@('preflight_walk','preflight_paths');Old='paths.final_units > rep->max_path_units ||';New='0 ||';Test='unit';Count=2},
  @{Name='transaction_path_limit';Files=@('preflight_walk','preflight_paths');Old='paths.transaction_units > rep->max_path_units';New='0';Test='unit';Count=2}
)
$results = @()
foreach ($m in $mutants) {
  $dir = Join-Path $root $m.Name; New-Item -ItemType Directory -Path $dir | Out-Null
  $mutationFiles = if ($m.ContainsKey('Files')) { $m.Files } else { @($m.File) }
  $compileFiles = if ($m.Test -eq 'unit') { @('preflight','preflight_walk','preflight_paths','paths') } else { @($m.File) }
  $count = 0
  $files = foreach ($key in $compileFiles) {
    $source = $sources[$key].Replace("`r`n","`n")
    if ($key -in $mutationFiles) {
      $count += Measure-WfAnchor $source $m.Old
      $source = Edit-WfAnchor $source $m.Old $m.New
    }
    $includes = @{'p0_source_guard.h'='tools/p0/p0_source_guard.h';'../../src/path/wf_path.h'='src/path/wf_path.h';'wf_xfer.h'='src/xfer/wf_xfer.h';'wf_preflight_internal.h'='src/xfer/wf_preflight_internal.h';'../path/wf_path.h'='src/path/wf_path.h'}
    foreach ($include in $includes.Keys) {
      $source = $source.Replace('#include "' + $include + '"','#include "' + (Join-Path $repo $includes[$include]).Replace('\','/') + '"')
    }
    $file = Join-Path $dir ($key + '.c'); [IO.File]::WriteAllText($file,$source)
    $file
  }
  $expectedCount = if($m.ContainsKey('Count')){$m['Count']}else{1}
  if ($count -ne $expectedCount) { throw "Mutant edit count changed: $($m.Name): $count" }
  $exe = Join-Path $dir 'test.exe'
  $runtime = Get-WfRuntime $Configuration $san
  $args = @('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,'/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
    "/I$repo/src","/I$repo/tests","/Fo$dir/","/Fe$exe") + $files
  if($m.Test -eq 'unit') { $args += Join-Path $repo 'tests/test_preflight_batch.c' }
  else {
    $args += $(if($m.Test -eq 'guard'){Join-Path $repo 'tests/test_source_guard_win.c'}else{Join-Path $repo 'tools/p0/p0_s08s10.c'})
    $args += @((Join-Path $repo 'src/xfer/wf_probe_win.c'),(Join-Path $repo 'src/fs/wf_enum_win.c'))
  }
  $args += @((Join-Path $buildRoot 'wfcore.lib'),(Join-Path $buildRoot 'p0_lab.lib'),(Join-Path $buildRoot 'p0_common.lib'),'bcrypt.lib','advapi32.lib')
  $args += $san.Compile
  if($san.Enabled){ $args += @('/link') + $san.Link }
  & $Compiler @args *> (Join-Path $dir 'build.log')
  if($LASTEXITCODE -ne 0) { throw "Mutant build failed, never counted as kill: $($m.Name) $dir/build.log" }
  $testArgs = if($m.Test -eq 'poc'){@('--guard-only')}else{@()}
  & $exe @testArgs *> (Join-Path $dir 'test.log')
  $results += Test-WfMutant $m.Name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $null)
