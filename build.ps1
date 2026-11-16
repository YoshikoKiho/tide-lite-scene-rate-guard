$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Zig = if ($env:ZIG_EXE -and (Test-Path $env:ZIG_EXE)) {
    $env:ZIG_EXE
} elseif (Get-Command zig -ErrorAction SilentlyContinue) {
    (Get-Command zig).Source
} else {
    Get-ChildItem -Path (Join-Path $Root '..\.toolchain') -Filter 'zig.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
$Src = Join-Path $Root 'src\chaoxid.cpp'
$ModDir = Join-Path $Root 'module'
$BinOut = Join-Path $ModDir 'bin\chaoxid'

if (-not $Zig -or -not (Test-Path $Zig)) {
    throw 'Zig toolchain not found. Set $env:ZIG_EXE or add zig to PATH.'
}

$BinDir = Split-Path -Parent $BinOut
if (-not (Test-Path $BinDir)) {
    New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
}

Write-Host "[1/4] Compiling chaoxid (aarch64-linux-musl, static C++17)..."
& $Zig c++ -target aarch64-linux-musl -static -std=c++17 -O2 -Wall -Wextra -Werror -s -o $BinOut $Src
if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed with exit code $LASTEXITCODE"
}

Write-Host "[2/4] Normalizing Unix LF line endings on scripts & module.prop..."
$LfTargets = @(
    (Join-Path $ModDir 'module.prop'),
    (Join-Path $ModDir 'service.sh'),
    (Join-Path $ModDir 'customize.sh'),
    (Join-Path $ModDir 'uninstall.sh')
)
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
foreach ($f in $LfTargets) {
    if (-not (Test-Path $f)) { throw "Missing module file: $f" }
    $txt = [System.IO.File]::ReadAllText($f, [System.Text.Encoding]::UTF8)
    $lf = $txt.Replace("`r`n", "`n").Replace("`r", "`n")
    [System.IO.File]::WriteAllText($f, $lf, $Utf8NoBom)
}

# Read version for output archive name
$PropText = [System.IO.File]::ReadAllText((Join-Path $ModDir 'module.prop'), [System.Text.Encoding]::UTF8)
$Ver = 'v1.0.0'
if ($PropText -match '(?m)^version=(.+)$') {
    $Ver = $Matches[1].Trim()
}

Write-Host "[3/4] Creating KernelSU flashable zip..."
$ZipPath = Join-Path $Root "chaoxi-$Ver.zip"
if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $Files = Get-ChildItem -Path $ModDir -Recurse -File
    foreach ($File in $Files) {
        $Rel = $File.FullName.Substring($ModDir.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $Archive,
            $File.FullName,
            $Rel,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $Archive.Dispose()
}

Write-Host "[4/4] Done: $ZipPath ($((Get-Item $ZipPath).Length) bytes)"
