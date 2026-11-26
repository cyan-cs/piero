# CI-only tool provisioning. Product builds gain no runtime dependency.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wf_download.ps1')
if ($env:GITHUB_ACTIONS -ne 'true' -or !$env:RUNNER_TEMP -or !$env:GITHUB_ENV -or !$env:GITHUB_PATH) {
    throw 'Run this provisioning script only in a GitHub Actions runner'
}
# Version, asset and SHA-256 are from the official LLVM release assets.
$version = '23.1.3'
$sha256 = '7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/clang+llvm-$version-x86_64-pc-windows-msvc.tar.zst"
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop | Select-Object -First 1).Source
$scratch = Join-Path $env:RUNNER_TEMP ('piero-llvm-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $scratch)
$download = if ($env:PIERO_LLVM_DOWNLOAD_DIR) { $env:PIERO_LLVM_DOWNLOAD_DIR } else { $scratch }
[void](New-Item -ItemType Directory -Path $download -Force)
$archive = Join-Path $download "llvm-$version-$sha256.tar.zst"
$archive = Get-WfVerifiedDownload $url $archive $sha256
& $sevenZip x -y "-o$scratch" $archive
if ($LASTEXITCODE) { throw 'LLVM zstd extraction failed' }
$tar = Join-Path $scratch ([IO.Path]::GetFileNameWithoutExtension($archive))
if (!(Test-Path -LiteralPath $tar -PathType Leaf)) { throw 'Expected LLVM tar archive missing' }
# Stable compiler paths let compiler-cache entries survive subsequent jobs.
$unpacked = Join-Path $env:RUNNER_TEMP "piero-llvm-$version"
& $sevenZip x -y "-o$unpacked" $tar
if ($LASTEXITCODE) { throw 'LLVM tar extraction failed' }
$compilers = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter clang-cl.exe)
if ($compilers.Count -ne 1) { throw 'LLVM compiler root is ambiguous or missing' }
$bin = $compilers[0].Directory.FullName
$llvmRoot = Split-Path -Parent $bin
$formatter = Join-Path $bin 'clang-format.exe'
$compilerVersion = & $compilers[0].FullName --version
if ($LASTEXITCODE -or $compilerVersion[0] -notmatch '^clang version 23\.1\.3(?:\s|$)') {
    throw 'Unexpected LLVM compiler version'
}
$formatVersion = & $formatter --version
if ($LASTEXITCODE -or $formatVersion -notmatch '^clang-format version 23\.1\.3(?:\s|$)') {
    throw 'Unexpected LLVM formatter version'
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::AppendAllText($env:GITHUB_ENV, "PIERO_LLVM_ROOT=$llvmRoot`n", $utf8)
[IO.File]::AppendAllText($env:GITHUB_PATH, "$bin`n", $utf8)
Write-Output "Verified LLVM $version for subsequent CI steps"
