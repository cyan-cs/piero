param(
  [string]$Compiler = 'clang-cl',
  [string]$Build = 'out/verify-agent',
  [string]$Configuration = 'Release'
)
# Each mutant weakens one rule of the verification policy / stage order. The
# pure test must fail for every one of them (exit 1 with a FAIL line); a mutant
# that does not build is an error, never a kill.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
Initialize-WfToolchain
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = [IO.Path]::GetFullPath($Build)
$san = Get-WfSanitizer $Compiler $buildRoot
$root = Join-Path $buildRoot ('verify-mutants-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$original = [IO.File]::ReadAllText((Join-Path $repo 'src/xfer/wf_verify.c'))
$mutants = @(
  @{Name='cross_volume_keeps_none';Old='out->method = WF_VERIFY_NONE == configured ? WF_VERIFY_SHA256 : configured;';New='out->method = configured;'},
  @{Name='removal_without_record';Old='(WF_STAGE_VERIFIED_RECORDED == m->stage ||';New='(WF_STAGE_VERIFIED == m->stage ||'},
  @{Name='removal_ignores_stop';Old='WF_STOP_NONE == m->stop;';New='true;'},
  @{Name='removal_without_read_back';Old='return m->plan.removes_source && m->plan.read_back &&';New='return m->plan.removes_source &&'},
  @{Name='mismatch_passes';Old='WF_VERDICT_MATCH == verdict, why);';New='true, why);'},
  @{Name='verify_before_durable';Old='step(m, WF_STAGE_DESTINATION_DURABLE, WF_STAGE_VERIFIED,';New='step(m, WF_STAGE_SOURCE_ACCEPTED, WF_STAGE_VERIFIED,'},
  @{Name='record_failure_ignored';Old='record_durable, WF_STOP_RECORD_FAILED);';New='record_durable || 1, WF_STOP_RECORD_FAILED);'},
  @{Name='rename_is_hashed';Old='case WF_KIND_MOVE_SAME_VOLUME: return WF_OK;';New='case WF_KIND_MOVE_SAME_VOLUME: out->read_back = true; return WF_OK;'},
  @{Name='copy_removes_source';Old='out->read_back = WF_VERIFY_NONE != configured;';New='out->read_back = WF_VERIFY_NONE != configured; out->removes_source = true;'},
  @{Name='source_rejection_ignored';Old='accepted, WF_STOP_SOURCE_REJECTED);';New='accepted || 1, WF_STOP_SOURCE_REJECTED);'},
  @{Name='flush_failure_ignored';Old='flushed, WF_STOP_NOT_DURABLE);';New='flushed || 1, WF_STOP_NOT_DURABLE);'}
)
$results = @()
foreach ($mutant in (@(@{Name='control';Control=$true}) + $mutants)) {
  $dir = Join-Path $root $mutant.Name
  New-Item -ItemType Directory -Path $dir | Out-Null
  if (!$mutant.Control) {
    $count = Measure-WfAnchor $original $mutant.Old
    if ($count -ne 1) { throw "Mutation anchor changed: $($mutant.Name): $count" }
  }
  $file = Join-Path $dir 'wf_verify.c'
  $text = if ($mutant.Control) { $original } else { Edit-WfAnchor $original $mutant.Old $mutant.New }
  [IO.File]::WriteAllText($file, $text)
  $exe = Join-Path $dir 'test_verify.exe'
  $runtime = Get-WfRuntime $Configuration $san
  $arguments = @('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,
    "/I$repo/src","/I$repo/src/xfer","/I$repo/tests","/I$buildRoot/generated","/Fo$dir/","/Fe$exe",
    (Join-Path $repo 'tests/test_verify.c'),$file,'/link')
  $arguments = Add-WfSanitizerArgs $arguments $san
  & $Compiler @arguments *> (Join-Path $dir 'build.log')
  if ($LASTEXITCODE -ne 0) { throw "Mutation did not build (not counted as killed): $($mutant.Name) $dir" }
  & $exe *> (Join-Path $dir 'test.log')
  if ($mutant.Control) {
    if ($LASTEXITCODE -ne 0 -or
        (Select-String -LiteralPath (Join-Path $dir 'test.log') -Pattern '^FAIL ' -CaseSensitive -Quiet)) {
      throw 'Unmodified verification control failed; no mutant verdict is valid'
    }
    Write-Output 'Unmodified verification control: PASS'
    continue
  }
  $results += Test-WfMutant $mutant.Name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $null)
