# Finds the Visual Studio installation instead of assuming where it is.
# Order: the environment of an already open developer shell, vswhere (which
# ships with the installer), then the usual folders of any version/edition.
# Returns the install root, or throws with what to do.
function Find-WfVisualStudio {
  if ($env:VSINSTALLDIR -and (Test-Path -LiteralPath $env:VSINSTALLDIR)) {
    return (Resolve-Path -LiteralPath $env:VSINSTALLDIR).Path.TrimEnd('\','/')
  }
  $pf86 = ${env:ProgramFiles(x86)}
  if ($pf86) {
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
      $found = & $vswhere -latest -products * -property installationPath 2>$null
      if ($found -and (Test-Path -LiteralPath ($found | Select-Object -First 1))) {
        return ($found | Select-Object -First 1)
      }
    }
  }
  foreach ($base in @($env:ProgramFiles, $pf86)) {
    if (!$base) { continue }
    $root = Join-Path $base 'Microsoft Visual Studio'
    if (!(Test-Path -LiteralPath $root)) { continue }
    $candidates = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending |
      ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue } |
      Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll') }
    if ($candidates) { return @($candidates)[0].FullName }
  }
  throw 'Visual Studio (with the C++ workload) was not found. Open a "Developer PowerShell for VS" and run this again, or install it.'
}

# Prefer the VS-bundled Ninja, as validate_publication.ps1 does. A PATH entry
# may point to a package alias that exists but cannot be executed here.
function Find-WfNinja([string]$VsRoot) {
  $bundled = Join-Path $VsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
  if (Test-Path -LiteralPath $bundled) { return $bundled }
  $onPath = Get-Command ninja -ErrorAction SilentlyContinue
  if ($onPath) { return $onPath.Source }
  throw 'ninja was not found (put it on PATH or install the Visual Studio CMake tools).'
}
