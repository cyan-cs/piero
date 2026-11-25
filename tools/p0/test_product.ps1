param([Parameter(Mandatory)][string]$HostExe,[Parameter(Mandatory)][string]$Product,
  [Parameter(Mandatory)][string]$Results)
$ErrorActionPreference='Stop'
$label='ctest-'+[guid]::NewGuid().ToString('N').Substring(0,8)
& $HostExe --scenario product --child $Product --results $Results --label $label
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
