# Local development validation only. Never publishes, sweeps fixtures or changes gates.
param(
  [ValidateSet('clang','msvc')][string]$Toolchain = 'clang',
  [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
  [switch]$Sanitize,
  [switch]$Analyze,
  [switch]$BuildOnly,
  [switch]$SafeOnly,
  [string]$JUnit,
  [string]$BuildDirectory = 'out/pub-clang',
  [ValidateRange(1,64)][int]$BuildJobs = 6, # lower both on a machine short of memory
  [ValidateRange(1,64)][int]$TestJobs = 2
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wf_toolchain.ps1')
Initialize-WfToolchain
. (Join-Path $PSScriptRoot 'wf_vs.ps1')
$vs = Find-WfVisualStudio
$ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $ninja)) { $ninja = Find-WfNinja $vs }
$compilerName = if ($Toolchain -eq 'clang') { 'clang-cl.exe' } else { 'cl.exe' }
$compiler = if ($Toolchain -eq 'clang' -and $env:PIERO_LLVM_ROOT) {
  # Keep VS environment import from selecting another LLVM in a CI job.
  $selected = Join-Path $env:PIERO_LLVM_ROOT 'bin/clang-cl.exe'
  if (!(Test-Path -LiteralPath $selected -PathType Leaf)) { throw 'Selected LLVM compiler is missing' }
  (Resolve-Path -LiteralPath $selected).Path
} else { (Get-Command $compilerName -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source }
$compiler = $compiler.Replace('\', '/')
$cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
  $cached = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_C_COMPILER:[^=]+=(.+)$'
  if ($cached) {
    $previous = $cached.Matches[0].Groups[1].Value.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($previous)) {
      if (![string]::Equals($previous, $compiler, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Compiler differs from the existing build. Use a fresh build directory.'
      }
      # Preserve CMake's spelling/case to avoid needless compiler-cache resets.
      $compiler = $previous
    }
  }
}
if ($Sanitize -and $Toolchain -ne 'clang') { throw 'Sanitizers require clang-cl' }
$args0 = @('-S','.', '-B',$BuildDirectory, '-G','Ninja',
  "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_C_COMPILER=$compiler",
  "-DCMAKE_BUILD_TYPE=$Configuration", "-DWF_SANITIZE=$($Sanitize.IsPresent.ToString().ToUpper())",
  "-DWF_ANALYZE=$($Analyze.IsPresent.ToString().ToUpper())")
& cmake @args0
if ($LASTEXITCODE) { throw 'Configure failed' }
& cmake --build $BuildDirectory -j $BuildJobs
if ($LASTEXITCODE) { throw 'Build failed' }
if (!$BuildOnly) {
  $testArgs = @('--test-dir', $BuildDirectory, '--output-on-failure', '-j', $TestJobs)
  if ($JUnit) { $testArgs += @('--output-junit', [IO.Path]::GetFullPath($JUnit)) }
  if ($SafeOnly) { $testArgs += @('-L', 'non-destructive') }
  & ctest @testArgs
  if ($LASTEXITCODE) { throw 'CTest failed; preserve evidence and report FAIL' }
}
