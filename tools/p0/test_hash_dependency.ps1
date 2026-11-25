param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Results,
  [string]$Collector)
$ErrorActionPreference='Stop'
if(!$Collector){$Collector=Join-Path $PSScriptRoot 'test_storage_evidence.ps1'}
# Reproduce a host where the optional Get-FileHash command is unavailable.
$null='{}' | ConvertFrom-Json
function global:Get-FileHash { throw 'Get-FileHash is intentionally unavailable in this regression test' }
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$vectors=Join-Path $Results ('sha-vectors-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $vectors | Out-Null
$abc=Join-Path $vectors 'literal[abc].txt'
$empty=Join-Path $vectors 'empty.txt'
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($abc),[byte[]]@(97,98,99))
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($empty),[byte[]]@())
if((Get-WfFileSha256 $abc) -ne 'BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD') {throw 'SHA256 abc vector failed'}
if((Get-WfFileSha256 $empty) -ne 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855') {throw 'SHA256 empty vector failed'}
$exclusive=[IO.File]::Open([IO.Path]::GetFullPath($abc),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$exclusive.Dispose()
& $Collector -Exe $Exe -Results $Results
if(!$?){throw 'Evidence dependency regression failed'}
