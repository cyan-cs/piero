# A restored cache entry is untrusted until the pinned release digest matches.
function Get-WfVerifiedDownload([string]$Url,[string]$Destination,[string]$Sha256) {
 if(!(Test-Path -LiteralPath $Destination -PathType Leaf)){
  Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Destination
 } else {
  Write-Host 'Reusing release archive; verifying SHA-256'
 }
 if((Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash -ne $Sha256){
  throw 'Release archive checksum mismatch; refusing extraction'
 }
 return $Destination
}
