# Copied into a prepared fixture. Run BY HAND inside an existing real terminal.
# This does not launch Windows Terminal, grant VM ownership, or mark tests PASS.
param(
  [ValidateSet('standard','nano','vim','custom')][string]$Profile='standard',
  [ValidateSet('auto','sixel','text')][string]$Graphics='auto',
  [ValidateSet('en','ja')][string]$Language='ja',
  [switch]$Setup
)
$ErrorActionPreference='Stop'
$owner = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot '.piero-manual-owner.json') | ConvertFrom-Json
$metadata = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'environment.json') | ConvertFrom-Json
$root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
if ($root -ne $owner.root) { throw 'Fixture location changed; stop' }
$repository = $metadata.repository
& python (Join-Path $repository 'tools/prepare_manual_validation.py') --verify $root
if ($LASTEXITCODE) { throw 'Fixture identity failed' }
if ((Get-FileHash -LiteralPath $metadata.product -Algorithm SHA256).Hash -ne $metadata.product_sha256) { throw 'Product changed; prepare a fresh fixture' }
$savedAppData=$env:APPDATA
$savedLocalData=$env:LOCALAPPDATA
$exitCode=$null
$pushed=$false
Write-Output 'Visual validation only. Filesystem/power-loss trials require a separately verified disposable VM, disks and snapshot.'
try {
  $env:APPDATA=Join-Path $root 'state/roaming'
  $env:LOCALAPPDATA=Join-Path $root 'state/local'
  Push-Location -LiteralPath (Join-Path $root 'data')
  $pushed=$true
  $productArgs=@('--language',$Language,'--config',(Join-Path $root "config/$Profile-$Graphics.ini"))
  if ($Setup) { $productArgs += '--setup' }
  $productArgs += (Join-Path $root 'data')
  & $metadata.product @productArgs
  $exitCode=$LASTEXITCODE
} finally {
  if ($pushed) { Pop-Location }
  $env:APPDATA=$savedAppData
  $env:LOCALAPPDATA=$savedLocalData
}
[ordered]@{product_sha256=$metadata.product_sha256;exit_code=$exitCode;profile=$Profile;graphics=$Graphics;language=$Language;setup=$Setup.IsPresent;
  manual_case_status='NOT RUN until recorded by the observer';windows_terminal_visual='NOT VERIFIED';timestamp_utc=[DateTime]::UtcNow.ToString('o')} |
  ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $root ('evidence/session-'+[guid]::NewGuid().ToString('N')+'.json'))
