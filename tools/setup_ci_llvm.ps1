# CI-only tool provisioning. Product builds gain no runtime dependency.
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or !$env:RUNNER_TEMP -or !$env:GITHUB_ENV -or !$env:GITHUB_PATH) {
    throw 'Run this provisioning script only in a GitHub Actions runner'
}
# Version, asset and SHA-256 are from the official LLVM release assets.
$version = '23.1.3'
$sha256 = '7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/clang+llvm-$version-x86_64-pc-windows-msvc.tar.zst"
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
$scratch = Join-Path $env:RUNNER_TEMP ('piero-llvm-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $scratch)
$archive = Join-Path $scratch 'llvm.tar.zst'
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $sha256) {
    throw 'LLVM archive checksum mismatch; refusing extraction'
}
& $sevenZip x -y "-o$scratch" $archive
if ($LASTEXITCODE) { throw 'LLVM zstd extraction failed' }
$tar = Join-Path $scratch 'llvm.tar'
if (!(Test-Path -LiteralPath $tar -PathType Leaf)) { throw 'Expected LLVM tar archive missing' }
$unpacked = Join-Path $scratch 'unpacked'
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
