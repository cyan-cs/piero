param([Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$Build,
  [ValidateSet('Debug','Release')][string]$Configuration='Debug', [string]$Evidence)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'owned_test_sources.ps1')
Initialize-WfToolchain
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$san=Get-WfSanitizer $Compiler $buildRoot
$root=Join-Path $buildRoot ('recovery-mutations-'+[guid]::NewGuid().ToString('N'))
$mutants = @(
  @{name='committed_nonce';file='owned';old='memcmp(nonce, o->record.nonce, sizeof nonce)';new='0';test='recovery'},
  @{name='committed_exact_size';file='owned';old='(uint64_t)size.EndOfFile.QuadPart != o->record.source_size';new='0';test='recovery'},
  @{name='committed_strangers';file='owned';old='ok = wfo_contents_match(o->directory, WF_OWNED_TXN_OWNER == o->kind, 0, report);';new='ok = (wfo_contents_match(o->directory, WF_OWNED_TXN_OWNER == o->kind, 0, report), 1);';test='recovery'},
  @{name='live_durable_callback';file='owned';old='if (!durable_temp_id(context, &o->record))';new='if ((durable_temp_id(context, &o->record), 0))';test='recovery'},
  @{name='abandon_exact_confirmation';file='store';old='wcscmp(id, typed)';new='0';test='store'},
  @{name='abandon_audit_before_delete';file='store';old='if (!log_event(s, id, "RECOVERY_ABANDON_INTENT", &report->error)) { goto done; }';new='if (0) { goto done; }';test='store'},
  @{name='abandon_target_io';file='store';old='if (!read_meta(s, id, &meta, &metadata, &report->error)) { return 0; }';new='if (!read_meta(s, id, &meta, &metadata, &report->error)) { return 0; } CloseHandle(CreateFileW(meta.volume_identity, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL));';test='store'},
  @{name='orphan_journal_policy';file='store';old='info->state = entry->intent ? WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED : WF_RECOVERY_LOCAL_JOURNAL_ONLY;';new='info->state = entry->intent ? WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED : WF_RECOVERY_LOCAL_JOURNAL_ONLY; info->action = WF_RECOVERY_ABANDON_PAIR;';test='partial'},
  @{name='orphan_metadata_policy';file='store';old='info->state = entry->intent ? WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED : WF_RECOVERY_LOCAL_METADATA_ONLY;';new='info->state = entry->intent ? WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED : WF_RECOVERY_LOCAL_METADATA_ONLY; info->action = WF_RECOVERY_ABANDON_PAIR;';test='partial'},
  @{name='audit_pending_action';file='store';old='info->action = WF_RECOVERY_ABANDON_AUDIT_ONLY;';new='info->action = WF_RECOVERY_ABANDON_HOLD;';test='partial'},
  @{name='original_query_error';file='store';old='info->error = meta_error && ERROR_FILE_NOT_FOUND != meta_error ? meta_error : journal_error;';new='info->error = ERROR_GEN_FAILURE;';test='partial'},
  @{name='short_write_refusal';file='store';old='if (wrote != count)';new='if (0)';test='partial'},
  @{name='unknown_audit_refusal';file='store';old='ok = audit_bytes(h, &bytes, &length, error) && parse_audit(s, bytes, length, error);';new='ok = audit_bytes(h, &bytes, &length, error);';test='partial'}
)
$results=@()
foreach($m in $mutants){
  $dir=Join-Path $root $m.name
  $sources=@(Copy-WfOwnedSources $repo (Join-Path $dir 'src'))
  $ownedFile=Join-Path $dir 'src/fs/owned_delete.c'
  $pathFile=Join-Path $dir 'src/path/wf_canonical_win.c'
  Copy-Item -LiteralPath (Join-Path $repo 'src/path/wf_canonical_win.c') -Destination $pathFile
  $exe=Join-Path $dir 'test.exe'
  $runtime=Get-WfRuntime $Configuration $san
  $base=@('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,'/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
    "/I$repo/src","/I$repo/tests","/Fo$dir/")

  $storeFile=Join-Path $dir 'wf_recovery_store.c'
  Copy-Item -LiteralPath (Join-Path $repo 'src/recovery/wf_recovery_store.c') -Destination $storeFile
  # Keep the source's original relative include base when copying this unit.
  $store=[IO.File]::ReadAllText($storeFile)
  $store=[regex]::Replace($store,'#include "([^"]+)"',{param($match)
    '#include "'+([IO.Path]::GetFullPath((Join-Path (Join-Path $repo 'src/recovery') $match.Groups[1].Value))).Replace('\','/')+'"'
  })
  [IO.File]::WriteAllText($storeFile,$store)
  if($m.file -eq 'owned'){Set-WfOwnedMutation $sources $m.old $m.new}
  else{Set-WfOwnedMutation @($storeFile) $m.old $m.new}
  $testArgs=@()
  $links=@('/link',(Join-Path $buildRoot 'p0_journal.lib'),(Join-Path $buildRoot 'p0_common.lib'),(Join-Path $buildRoot 'p0_lab.lib'),'bcrypt.lib','advapi32.lib')
  $links+=$san.Link
  $base+=$san.Compile
  if($m.test -eq 'partial'){
    $shim='#define WF_RECOVERY_STORE_SOURCE "'+$storeFile.Replace('\','/')+'"'+"`n"+
      '#define WF_RECOVERY_OWNED_SOURCE "'+$ownedFile.Replace('\','/')+'"'+"`n"+
      '#include "'+(Join-Path $repo 'tests/test_recovery_partial.c').Replace('\','/')+'"'+"`n"
    $driver=Join-Path $dir 'partial_variant.c';[IO.File]::WriteAllText($driver,$shim)
    Invoke-WfOwnedFaultBuild $Compiler $base $sources $driver (Join-Path $repo 'tests/wf_recovery_fault_shim.h') $links $exe $dir @($pathFile,(Join-Path $repo 'src/recovery/wf_recovery.c'))
  }else{
    $driver=Join-Path $repo 'tests/test_recovery_win.c'
    if($m.test -eq 'store'){
      $shim='#define WF_RECOVERY_STORE_SOURCE "'+$storeFile.Replace('\','/')+'"'+"`n"+
        '#include "'+(Join-Path $repo 'tests/test_recovery_store.c').Replace('\','/')+'"'+"`n"
      $driver=Join-Path $dir 'store_variant.c';[IO.File]::WriteAllText($driver,$shim)
    }
    $arguments=$base+@("/Fe$exe",$driver)+$sources+@($pathFile,(Join-Path $repo 'src/recovery/wf_recovery.c'))+$links
    & $Compiler @arguments *> (Join-Path $dir 'build.log')
  }

  if($LASTEXITCODE -ne 0){throw "Mutation compile failed (never a kill): $($m.name); $dir/build.log"}
  & $exe @testArgs *> (Join-Path $dir 'test.log')
  $results += Test-WfMutant $m.name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $Evidence)
