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
$root=Join-Path $buildRoot ('mutations-'+[guid]::NewGuid().ToString('N'))
$mutants = @(
  @{Name='temp_durability'; File='owned'; Old='!r->temp_id_durable'; New='0'; Test='unit'},
  @{Name='uuid'; File='owned'; Old='memcmp(r->intent_txn, r->txn, 16)'; New='0'; Test='unit'},
  @{Name='independent_volume'; File='path'; Old='a->volume != b->volume'; New='0'; Test='unit'},
  @{Name='ordinal_path'; File='path'; Old='if (i < 47) {'; New='if (1) {'; Test='unit'},
  @{Name='single_link'; File='owned'; Old='(!directory && 1 != standard.NumberOfLinks)'; New='0'; Test='adversarial'},
  @{Name='size_bound'; File='owned'; Old='(uint64_t)size.EndOfFile.QuadPart > o->record.source_size'; New='0'; Test='adversarial'},
  @{Name='nonce'; File='owned'; Old='memcmp(nonce, o->record.nonce, 32)'; New='0'; Test='adversarial'},
  @{Name='object_identity'; File='owned'; Old='!wfo_id_equal(&id, expected)'; New='0'; Test='faults'},
  @{Name='reparse'; File='owned'; Old='(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)'; New='0'; Test='faults'},
  @{Name='unexpected_children'; File='owned'; Old='ok = wfo_contents_match(o->directory, 1, INVALID_HANDLE_VALUE != o->payload, report);'; New='ok = (wfo_contents_match(o->directory, 1, INVALID_HANDLE_VALUE != o->payload, report), 1);'; Test='adversarial'},
  @{Name='delete_reverification'; File='owned'; Old='!(WF_OWNED_COPY_PAYLOAD == owned->kind ? wfo_transaction_matches(owned, report) : wfo_committed_matches(owned, report))'; New='0'; Test='faults'},
  @{Name='retry_bound'; File='owned'; Old='report->open_attempts = owned->open_attempts; for (i = 0; i < WF_ARRAY_LEN(wfo_retry_ms); i++)'; New='report->open_attempts = owned->open_attempts; for (i = 0; i < 5; i++)'; Test='faults'},
  @{Name='retry_schedule'; File='owned'; Old='{0, 50, 100, 200, 400, 800}'; New='{0, 50, 100, 200, 401, 800}'; Test='faults'},
  @{Name='closed_kind'; File='owned'; Old='!out || WF_OWNED_COPY_PAYLOAD != kind'; New='!out || 0'; Test='faults'}
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

  if($m.file -eq 'owned'){Set-WfOwnedMutation $sources $m.old $m.new}
  else{Set-WfOwnedMutation @($pathFile) $m.old $m.new}
  $testArgs=if($m.test -eq 'adversarial'){@('--adversarial-only')}else{@()}
  $links=@('/link',(Join-Path $buildRoot 'p0_journal.lib'),(Join-Path $buildRoot 'p0_common.lib'),'bcrypt.lib','advapi32.lib')
  $links+=$san.Link
  $base+=$san.Compile
  if($m.test -eq 'faults'){
    $shim='#define WF_OWNED_DELETE_SOURCE "'+$ownedFile.Replace('\','/')+'"'+"`n"+
      '#define WF_CANONICAL_SOURCE "'+$pathFile.Replace('\','/')+'"'+"`n"+
      '#include "'+(Join-Path $repo 'tests/test_owned_faults.c').Replace('\','/')+'"'+"`n"
    $driver=Join-Path $dir 'fault_variant.c';[IO.File]::WriteAllText($driver,$shim)
    Invoke-WfOwnedFaultBuild $Compiler $base $sources $driver (Join-Path $repo 'tests/wf_owned_fault_shim.h') $links $exe $dir @((Join-Path $repo 'src/recovery/wf_recovery.c'))
  }else{
    $test=if($m.test -eq 'unit'){'tests/test_owned_record.c'}else{'tests/test_storage_win.c'}
    $arguments=$base+@("/Fe$exe",(Join-Path $repo $test))+$sources+@($pathFile,(Join-Path $repo 'src/recovery/wf_recovery.c'))+$links
    & $Compiler @arguments *> (Join-Path $dir 'build.log')
  }

  if($LASTEXITCODE -ne 0){throw "Mutation compile failed (never a kill): $($m.name); $dir/build.log"}
  & $exe @testArgs *> (Join-Path $dir 'test.log')
  $results += Test-WfMutant $m.name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $Evidence)
