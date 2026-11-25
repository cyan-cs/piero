# The test variant uses the same seven separately compiled ownership units.
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
function Copy-WfOwnedSources([string]$Repository,[string]$Destination) {
  $names=@('owned_common.c','owned_payload.c','owned_committed.c','owned_local.c','owned_live.c','owned_log.c','owned_delete.c')
  foreach($subdir in @('fs','path','diagnostics')){New-Item -ItemType Directory -Path (Join-Path $Destination $subdir) -Force | Out-Null}
  foreach($header in @('fs/owned_delete.h','fs/owned_internal.h','path/wf_canonical_win.h','diagnostics/wf_diag_format.h')){
    Copy-Item -LiteralPath (Join-Path $Repository ('src/'+$header)) -Destination (Join-Path $Destination $header)
  }
  foreach($name in $names){
    $path=Join-Path $Destination ('fs/'+$name)
    Copy-Item -LiteralPath (Join-Path $Repository ('src/fs/'+$name)) -Destination $path
    $path
  }
}
function Set-WfOwnedMutation([string[]]$Sources,[string]$Old,[string]$New) {
  $locations=@()
  foreach($file in $Sources){
    $source=[IO.File]::ReadAllText($file)
    $count=Measure-WfAnchor $source $Old
    for($i=0;$i -lt $count;$i++){$locations+=$file}
  }
  if($locations.Count -ne 1){throw "Expected one mutation location, found $($locations.Count): $Old"}
  $source=Edit-WfAnchor ([IO.File]::ReadAllText($locations[0])) $Old $New
  [IO.File]::WriteAllText($locations[0],$source)
}
function Invoke-WfOwnedFaultBuild([string]$Compiler,[string[]]$Base,[string[]]$Sources,
  [string]$Driver,[string]$Header,[string[]]$Links,[string]$Exe,[string]$Directory,[string[]]$Extra) {
  $proofs=@($Sources | Where-Object { (Split-Path -Leaf $_) -ne 'owned_delete.c' })
  $arguments=$Base+@('/c',"/FI$Header")+$proofs
  & $Compiler @arguments *> (Join-Path $Directory 'build.log')
  if($LASTEXITCODE -ne 0){throw 'Fault proof module compilation failed, never a mutant kill'}
  $objects=@($proofs | ForEach-Object { Join-Path $Directory (([IO.Path]::GetFileNameWithoutExtension($_))+'.obj') })
  $arguments=$Base+@("/Fe$Exe",$Driver)+$Extra+$objects+$Links
  & $Compiler @arguments *> (Join-Path $Directory 'driver-build.log')
  $code=$LASTEXITCODE
  Get-Content -LiteralPath (Join-Path $Directory 'driver-build.log') | Out-File -Append -Encoding unicode -LiteralPath (Join-Path $Directory 'build.log')
  if($code -ne 0){throw 'Fault test driver compilation failed, never a mutant kill'}
  $global:LASTEXITCODE=0
}
