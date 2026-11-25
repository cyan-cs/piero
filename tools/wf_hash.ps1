# SHA-256 without dependence on PowerShell's optional Get-FileHash function.
# Literal file paths only; both the stream and algorithm have one owner.
function Get-WfFileSha256([Parameter(Mandatory)][string]$LiteralPath) {
  $stream=$null
  $algorithm=[Security.Cryptography.SHA256]::Create()
  try {
    $stream=[IO.File]::Open([IO.Path]::GetFullPath($LiteralPath),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')
  } finally {
    if($stream){$stream.Dispose()}
    $algorithm.Dispose()
  }
}
