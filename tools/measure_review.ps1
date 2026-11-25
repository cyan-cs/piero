[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
. (Join-Path $PSScriptRoot 'wf_toolchain.ps1')
Initialize-WfToolchain
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
$output = Join-Path $repo "out/review-bench-$Tag"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = (Get-Command clang-cl.exe -ErrorAction Stop).Source
$linker = Join-Path (Split-Path -Parent $compiler) 'lld-link.exe'
$units = @((Join-Path $PSScriptRoot 'bench_review.c'),
    (Join-Path $source 'src/xfer/wf_probe_win.c'),
    (Join-Path $source 'src/fs/wf_enum_win.c'))
$objects = @()
foreach ($unit in $units) {
    $object = Join-Path $output (([IO.Path]::GetFileNameWithoutExtension($unit)) + '.obj')
    & $compiler /nologo /O2 /MD /std:c17 /utf-8 /DUNICODE /D_UNICODE "/I$source/src" "/I$repo/tools" /c $unit "/Fo$object"
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $unit" }
    $objects += $object
}
$exe = Join-Path $output 'bench.exe'
$linkArgs = $objects + @("/out:$exe", "/libpath:$build", 'wfefile.lib',
    'wfcopyengine.lib', 'wfjournal.lib', 'wfverify.lib', 'wfintegrity.lib',
    'wfstorage.lib', 'wfcore.lib', 'p0_lab.lib', 'p0_common.lib',
    'advapi32.lib', 'bcrypt.lib', 'ole32.lib', 'shell32.lib', 'uuid.lib',
    'user32.lib', 'psapi.lib', '/subsystem:console')
& $linker @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
& $exe | Tee-Object -FilePath (Join-Path $output 'measurements.csv')
if ($LASTEXITCODE -ne 0) { throw 'Benchmark or owned-fixture cleanup failed.' }
