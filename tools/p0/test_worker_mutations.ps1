param([Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$Build,
  [ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
Initialize-WfToolchain
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$san=Get-WfSanitizer $Compiler $buildRoot
$root=Join-Path $buildRoot ('worker-mutants-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$results=@()
$workers=@(
  @{name='xfer';file='src/xfer/wf_xferworker.c';test='tests/test_xfer_lifetime.c';macro='WF_XFERWORKER_SOURCE';core=$true;coreInTest=$true},
  @{name='preview';file='src/preview/wf_previewworker.c';test='tests/test_preview_lifetime.c';macro='WF_PREVIEWWORKER_SOURCE';core=$true},
  @{name='fs';file='src/fs/wf_fsworker.c';test='tests/test_fs_lifetime.c';macro='WF_FSWORKER_SOURCE';core=$true}
)
foreach($worker in $workers){
  $original=[IO.File]::ReadAllText((Join-Path $repo $worker.file)).Replace("`r`n","`n")
  $coreFile='src/async/wf_workcore.c'
  $coreOriginal=[IO.File]::ReadAllText((Join-Path $repo $coreFile)).Replace("`r`n","`n")
  if($worker.core){
    # the stop and the release are the shared core's: the timeout report is
    # mutated there, and the worker's own call to release in its thread
    $lateOld="`twf_wc_release(&w->core, worker_free, w);`n`treturn 0;"
    $mutants=@(
      @{name='late_release';old=$lateOld;new="`t(void)w;`n`treturn 0;"},
      @{name='timeout_report';old='WAIT_TIMEOUT == wait ? WF_E_TIMEOUT : WF_E_OS';new='WAIT_TIMEOUT == wait ? WF_OK : WF_E_OS';target='core'}
    )
  } else {
    $mutants=@(
      @{name='late_release';old="`tworker_release(w);`n`treturn 0;";new="`t(void)w;`n`treturn 0;"},
      @{name='timeout_report';old='WAIT_TIMEOUT == wait ? WF_E_TIMEOUT : WF_E_OS';new='WAIT_TIMEOUT == wait ? WF_OK : WF_E_OS'}
    )
  }
  if($worker.name -eq 'xfer'){
    $mutants+=@{name='replacement_count';old='w->core.stats.superseded_requests++;';new='(void)w->core.stats.superseded_requests;'}
  }
  foreach($mutant in $mutants){
    $name=$worker.name+'_'+$mutant.name
    $inCore=('core' -eq $mutant.target)
    $text=if($inCore){$coreOriginal}else{$original}
    if((Measure-WfAnchor $text $mutant.old) -ne 1){throw "Mutation anchor mismatch: $name"}
    $dir=Join-Path $root $name
    New-Item -ItemType Directory -Path $dir | Out-Null
    $source=Edit-WfAnchor $text $mutant.old $mutant.new
    $base=Split-Path -Parent (Join-Path $repo $(if($inCore){$coreFile}else{$worker.file}))
    $source=[regex]::Replace($source,'#include "([^"]+)"',{param($match)
      '#include "'+([IO.Path]::GetFullPath((Join-Path $base $match.Groups[1].Value))).Replace('\','/')+'"'
    })
    $file=Join-Path $dir $(if($inCore){'core.c'}else{'worker.c'})
    [IO.File]::WriteAllText($file,$source)
    $workerSource=if($inCore){(Join-Path $repo $worker.file)}else{$file}
    $shim=$(if($inCore -and $worker.coreInTest){'#define WF_WORKCORE_SOURCE "'+$file.Replace('\','/')+'"'+"`n"}else{''})+
      '#define '+$worker.macro+' "'+$workerSource.Replace('\','/')+'"'+"`n"+
      '#include "'+(Join-Path $repo $worker.test).Replace('\','/')+'"'+"`n"
    $test=Join-Path $dir 'test.c'
    [IO.File]::WriteAllText($test,$shim)
    $exe=Join-Path $dir 'test.exe'
    $runtime=Get-WfRuntime $Configuration $san
    $arguments=@('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,'/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',
      "/I$repo/src","/I$repo/tests","/Fo$dir/","/Fe$exe",$test,'/link',
      (Join-Path $buildRoot 'wfarchive.lib'),(Join-Path $buildRoot 'src/third_party/wf_libarchive.lib'),(Join-Path $buildRoot 'src/third_party/wf_zlib.lib'),(Join-Path $buildRoot 'src/third_party/wf_lzma.lib'),(Join-Path $buildRoot 'src/third_party/wf_zstd.lib'),(Join-Path $buildRoot 'wfcore.lib'),(Join-Path $buildRoot 'p0_lab.lib'),(Join-Path $buildRoot 'p0_common.lib'),'bcrypt.lib','advapi32.lib','user32.lib')
    # the mutated core goes in as a source; the library's copy of it is then a duplicate, and the first (mutated) one is used. A mutant that did not take effect would survive, so this cannot hide anything.
    if($inCore -and -not $worker.coreInTest){$at=[array]::IndexOf($arguments,'/link');$arguments=$arguments[0..($at-1)]+@($file)+$arguments[$at..($arguments.Count-1)]+@('/FORCE:MULTIPLE')}
    $arguments=Add-WfSanitizerArgs $arguments $san
    $saved=$ErrorActionPreference
    try{$ErrorActionPreference='Continue';& $Compiler @arguments *> (Join-Path $dir 'build.log');$built=$LASTEXITCODE}
    finally{$ErrorActionPreference=$saved}
    if($built -ne 0){throw "Mutation compile failed (not killed): $name"}
    & $exe *> (Join-Path $dir 'test.log')
    $results += Test-WfMutant $name $dir $LASTEXITCODE
  }
}
exit (Complete-WfMutations $results $root $null)
