$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$projRoot = Split-Path -Parent $root
$llvmBin = @(
    "D:\Projeler\tools\llvm-mingw\bin",
    (Join-Path $projRoot "tools\llvm-mingw\bin")
) | Where-Object { Test-Path (Join-Path $_ "clang++.exe") } | Select-Object -First 1

if (-not $llvmBin) {
    $found = Get-ChildItem -Path $projRoot -Filter "clang++.exe" -Recurse -Depth 4 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $llvmBin = $found.DirectoryName }
}
if (-not $llvmBin) {
    throw "llvm-mingw clang++.exe bulunamadi."
}

$clang = Join-Path $llvmBin "clang++.exe"
$windres = Join-Path $llvmBin "x86_64-w64-mingw32-windres.exe"

$buildDir = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$resObj = Join-Path $buildDir "app.res.o"
$rcFile = Join-Path $root "assets\app.rc"
& $windres -I (Join-Path $root "assets") $rcFile -O coff -o $resObj
if ($LASTEXITCODE -ne 0) {
    throw "Resource derleme hatasi: $LASTEXITCODE"
}

$sources = Get-ChildItem (Join-Path $root "src") -Filter "*.cpp" | ForEach-Object { $_.FullName }
$outExe = Join-Path $root "AnlikKlip.exe"

$args = @(
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-DUNICODE",
    "-D_UNICODE",
    "-D_WIN32_WINNT=0x0A00",
    "-DWINVER=0x0A00",
    "-DNTDDI_VERSION=0x0A000008",
    "-mwindows",
    "-municode",
    "-static"
) + $sources + @(
    $resObj,
    "-o", $outExe,
    "-ld3d11",
    "-ldxgi",
    "-lmfplat",
    "-lmf",
    "-lmfplay",
    "-lmfreadwrite",
    "-lmfuuid",
    "-lstrmiids",
    "-lole32",
    "-loleaut32",
    "-luuid",
    "-lpropsys",
    "-lcomctl32",
    "-lshell32",
    "-ladvapi32",
    "-luser32",
    "-lgdi32",
    "-lgdiplus",
    "-lwinmm",
    "-lpsapi",
    "-lws2_32",
    "-liphlpapi",
    "-lwinhttp"
)

& $clang @args
if ($LASTEXITCODE -ne 0) {
    throw "Derleme basarisiz (kod: $LASTEXITCODE)"
}

$kb = [Math]::Round((Get-Item $outExe).Length / 1KB, 1)
Write-Host "Derleme basarili: $outExe ($kb KB)"
