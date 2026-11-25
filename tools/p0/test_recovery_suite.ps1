param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Results)
$ErrorActionPreference='Stop'
$run='ctest-'+[guid]::NewGuid().ToString('N')
& $Exe $Results $run
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
