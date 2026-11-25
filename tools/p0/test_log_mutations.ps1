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
$root=Join-Path $buildRoot ('log-mutations-'+[guid]::NewGuid().ToString('N'))
$mutants=@(
  @{name='record_crc';old='if (!wf_diag_records_valid(bytes, size)) { return 0; }';new='if ((wf_diag_records_valid(bytes, size), 0)) { return 0; }'},
  @{name='header_crc';old='header.crc32 != wf_diag_header_crc(&header, o->parent_path)';new='0'},
  @{name='marker_crc';old='marker.crc32 != wf_diag_marker_crc(&marker, o->parent_path)';new='0'},
  @{name='object_nonce';old='memcmp(header.object_nonce, r->object_nonce, 32)';new='0'},
  @{name='marker_nonce';old='memcmp(marker.nonce, r->root_nonce, 32)';new='0'},
  @{name='header_root_nonce';old='memcmp(header.root_nonce, r->root_nonce, 32)';new='0'},
  @{name='generation';old='header.generation != r->generation';new='0'},
  @{name='slot';old='header.slot != r->slot';new='0'},
  @{name='single_link';old='(!directory && 1 != standard.NumberOfLinks)';new='0'},
  @{name='header_root_id';old='!wfo_id_equal(&header.root, &r->root)';new='0'},
  @{name='header_marker_id';old='!wfo_id_equal(&header.marker, &r->marker)';new='0'},
  @{name='header_object_id';old='!wfo_id_equal(&header.object, &r->object)';new='0'},
  @{name='path_bound';old='header.path_units != units';new='0'},
  @{name='reserved';old='header.reserved ||';new='0 ||'},
  @{name='utf8';old='if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, (int)record.bytes, NULL, 0))';new='if ((MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, (int)record.bytes, NULL, 0), 0))'}
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

  $target=Join-Path $dir ('src/fs/'+$(if($m.name -eq 'single_link'){'owned_common.c'}else{'owned_log.c'}))
  Set-WfOwnedMutation @($target) $m.old $m.new
  $testArgs=@()
  $arguments=$base+@("/Fe$exe",(Join-Path $repo 'tests/test_recovery_log.c'))+$sources+@($pathFile,'/link',
    (Join-Path $buildRoot 'p0_common.lib'),(Join-Path $buildRoot 'p0_lab.lib'),'bcrypt.lib','advapi32.lib')
  $arguments=Add-WfSanitizerArgs $arguments $san
  & $Compiler @arguments *> (Join-Path $dir 'build.log')

  if($LASTEXITCODE -ne 0){throw "Mutation compile failed (never a kill): $($m.name); $dir/build.log"}
  & $exe @testArgs *> (Join-Path $dir 'test.log')
  $results += Test-WfMutant $m.name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $Evidence)
