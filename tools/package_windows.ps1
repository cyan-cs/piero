# Local build/package only. Never installs, publishes, signs or modifies PATH.
param(
  [string]$Version='0.0.3-dev',
  [string]$BuildDirectory='out/package-build-new',
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [ValidateSet('clang','msvc')][string]$Toolchain='clang',
  [switch]$Installer
)
$ErrorActionPreference='Stop'
$repository=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $repository
foreach ($candidate in @($BuildDirectory,$OutputDirectory)) {
  $resolved=[IO.Path]::GetFullPath((Join-Path $repository $candidate))
  if (!$resolved.StartsWith((Join-Path $repository 'out')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $resolved)) {
    throw 'Choose NEW build/output directories under workspace out/'
  }
}
. (Join-Path $PSScriptRoot 'wf_toolchain.ps1')
Initialize-WfToolchain
. (Join-Path $PSScriptRoot 'wf_vs.ps1')
$vs=Find-WfVisualStudio
$ninja=Find-WfNinja $vs
$compiler=(Get-Command $(if ($Toolchain -eq 'clang') {'clang-cl.exe'} else {'cl.exe'}) -ErrorAction Stop | Select-Object -First 1).Source
$languagePython = & python -c 'import sys; print(sys.executable)'
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $languagePython)) { throw 'Python 3 is required for the language catalog generator' }
& cmake -S . -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$compiler" "-DPython3_EXECUTABLE=$languagePython" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DWF_SANITIZE=OFF -DWF_ANALYZE=OFF -DBUILD_TESTING=OFF "-DPIERO_VERSION=$Version"
if ($LASTEXITCODE) { throw 'Package configure failed' }
& cmake --build $BuildDirectory --target piero piero_path -j 6
if ($LASTEXITCODE) { throw 'Package build failed' }
& python tools/package_windows.py --build $BuildDirectory --output $OutputDirectory
if ($LASTEXITCODE) { throw 'Package validation failed' }
if ($Installer) {
  $iscc=(Get-Command ISCC.exe -ErrorAction Stop | Select-Object -First 1).Source
  $payload=(Resolve-Path (Join-Path $OutputDirectory 'payload')).Path
  $output=(Resolve-Path $OutputDirectory).Path
  & $iscc "/DPackageDir=$payload" "/DPieroVersion=$Version" "/DOutputPath=$output" packaging/piero.iss
  if ($LASTEXITCODE) { throw 'Inno Setup compilation failed' }
} else {
  Write-Output 'Installer NOT BUILT (use -Installer with Inno Setup on PATH). ZIP is an unsigned local candidate, not a release.'
}
