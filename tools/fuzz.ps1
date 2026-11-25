# libFuzzer + ASan harnesses for the pure parsers (tests/fuzz/fuzz_*.c).
# Usage: pwsh tools/fuzz.ps1 [-Seconds 60] [-Only imgcheck,dwire]
# Corpus and crashes land in out/fuzz/<name>/; a crash input is kept as
# crash-* there and should be copied to tests/fuzz/regress/ with a test.
param([int]$Seconds = 60, [string[]]$Only = @())

$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # -File passes a comma list as one string
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out/fuzz'
New-Item -ItemType Directory -Force $out | Out-Null

$i18n = Get-ChildItem "$root/src/i18n/*.c" | ForEach-Object { "src/i18n/$($_.Name)" }
$common = @('src/base/wf_buf.c', 'src/text/wf_sanitize.c', 'src/text/wf_utf.c', 'src/text/wf_width.c', 'src/path/wf_path.c')
$targets = @{
    enc    = @('src/edit/wf_eenc.c')
    exline = @('src/input/wf_exline.c', 'src/fs/wf_dir.c', 'src/archive/wf_vpath.c') + $common + $i18n
    text   = @('src/archive/wf_vpath.c', 'src/path/wf_name.c') + $common
    config = @('src/config/wf_config.c', 'src/config/wf_ini_edit.c', 'src/render/wf_theme.c', 'src/input/wf_keymap.c', 'src/input/wf_commands.c', 'src/input/wf_stroke.c', 'src/edit/wf_ekeymap.c') + $common + $i18n
    imgcheck = @('src/preview/wf_image_check.c')
    dwire  = @('src/preview/wf_decode_wire.c')
    fuzzy  = @('src/search/wf_fuzzy.c')
    pixels = @('src/preview/wf_pixmap.c', 'src/render/wf_sixel.c', 'src/base/wf_buf.c')
}

# The ASan runtime is a DLL under /MD; let the harness find it.
$env:PATH = "$((Get-Item "$env:ProgramFiles/LLVM/lib/clang/*/lib/windows" | Select-Object -First 1).FullName);$env:PATH"
$failed = $false
foreach ($name in $targets.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    $exe = Join-Path $out "fuzz_$name.exe"
    $srcs = @("tests/fuzz/fuzz_$name.c") + $targets[$name] | ForEach-Object { Join-Path $root $_ }
    & clang-cl /nologo /std:c17 /Zi /O1 /MD "-fsanitize=fuzzer,address" "-I$root/src" @srcs "/Fe:$exe" | Out-Host
    if ($LASTEXITCODE) { throw "build of fuzz_$name failed" }
    $dir = Join-Path $out $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Push-Location $dir
    & $exe "-max_total_time=$Seconds" -max_len=4096 -print_final_stats=1 $dir 2>&1 | Select-Object -Last 12 | Out-Host
    if ($LASTEXITCODE) { Write-Host "FUZZ FAIL: $name"; $failed = $true }
    Pop-Location
}
if ($failed) { exit 1 }
