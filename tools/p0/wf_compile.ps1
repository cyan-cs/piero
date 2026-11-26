# Cache only compilation. Linking and every control/mutant execution stay fresh.
function Invoke-WfMutationBuild([string]$Compiler,[string[]]$Arguments,[string]$Directory) {
 $log=Join-Path $Directory 'build.log'
 if(!$env:PIERO_COMPILER_CACHE){
  & $Compiler @Arguments *> $log
  return $LASTEXITCODE
 }
 $at=[array]::IndexOf($Arguments,'/link')
 if($at -lt 0){throw 'Mutation build requires an explicit /link boundary'}
 $front=@($Arguments[0..($at-1)])
 $sources=@($front | Where-Object {$_ -notmatch '^[/\-]' -and $_ -match '\.c$'})
 # Absolute Windows paths begin with a drive letter. Forced includes are flags.
 if(!$sources.Count){throw 'Mutation build has no C sources'}
 $output=@($front | Where-Object {$_ -like '/Fe*'})
 if($output.Count -ne 1){throw 'Mutation build requires one executable output'}
 $options=@($front | Where-Object {$_ -notin $sources -and $_ -notlike '/Fo*' -and $_ -notlike '/Fe*'})
 $objects=@()
 [IO.File]::WriteAllText($log,'')
 for($i=0;$i -lt $sources.Count;$i++){
  $object=Join-Path $Directory ("$i-"+[IO.Path]::GetFileNameWithoutExtension($sources[$i])+'.obj')
  $compile=$options+@('/c',"/Fo$object",$sources[$i])
  & $env:PIERO_COMPILER_CACHE $Compiler @compile *>> $log
  $code=$LASTEXITCODE
  if($code -ne 0){return $code}
  $objects+=$object
 }
 $link=@('/nologo',$output[0])+$objects+@($Arguments[$at..($Arguments.Count-1)])
 & $Compiler @link *>> $log
 return $LASTEXITCODE
}
