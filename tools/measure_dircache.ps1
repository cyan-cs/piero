[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceRoot,
      [Parameter(Mandatory)][string]$BuildRoot,
      [Parameter(Mandatory)][string]$Fixture,
      [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag,
      [switch]$Cache)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wf_toolchain.ps1')
Initialize-WfToolchain
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo "out/dircache-review/$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = (Get-Command clang-cl.exe).Source
$linker = Join-Path (Split-Path -Parent $compiler) 'lld-link.exe'
foreach ($kind in @('dircache', 'dir_enum')) {
    $units = @((Join-Path $PSScriptRoot "bench_$kind.c"))
    if ($kind -eq 'dir_enum') { $units += Join-Path $source 'src/fs/wf_enum_win.c' }
    $objects = @()
    foreach ($unit in $units) {
        $object = Join-Path $output (([IO.Path]::GetFileNameWithoutExtension($unit)) + '.obj')
        $options = @('/nologo','/O2','/MD','/std:c17','/utf-8','/DUNICODE','/D_UNICODE',"/I$source/src")
        if ($Cache) { $options += '/DWF_BENCH_DIRCACHE' }
        & $compiler @options /c $unit "/Fo$object"
        if ($LASTEXITCODE) { throw "Compile failed: $unit" }
        $objects += $object
    }
    $exe = Join-Path $output "$kind.exe"
    & $linker @objects "/out:$exe" "/libpath:$build" wfcore.lib advapi32.lib user32.lib shell32.lib ole32.lib uuid.lib psapi.lib /subsystem:console
    if ($LASTEXITCODE) { throw "Link failed: $kind" }
    if ($kind -eq 'dir_enum') { & $exe $fixturePath | Tee-Object -FilePath (Join-Path $output "$kind.csv") }
    else { & $exe | Tee-Object -FilePath (Join-Path $output "$kind.csv") }
    if ($LASTEXITCODE) { throw "Benchmark failed: $kind" }
}
