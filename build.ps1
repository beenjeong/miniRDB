# Builds miniRDB into build\minirdb.exe.
# Prefers MSVC (cl.exe) via vcvarsall.bat; falls back to gcc if cl isn't
# available but gcc is on PATH.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$srcDir = Join-Path $root "src"
$buildDir = Join-Path $root "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

function Find-VcVarsAll {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $instPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($instPath) {
            $candidate = Join-Path $instPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $known = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "BuildTools\VC\Auxiliary\Build\vcvarsall.bat" } |
        Where-Object { Test-Path $_ }
    if ($known) { return $known | Select-Object -First 1 }
    return $null
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    $vcvarsall = Find-VcVarsAll
    if ($vcvarsall) {
        Write-Host "Using MSVC via $vcvarsall"
        $sources = (Get-ChildItem "$srcDir\*.c" | ForEach-Object { $_.FullName }) -join " "
        $exe = Join-Path $buildDir "minirdb.exe"
        $cmd = "call `"$vcvarsall`" x64 >nul && cl.exe /nologo /W3 /std:c17 /D_CRT_SECURE_NO_WARNINGS /Zi /I`"$srcDir`" $sources /Fe:`"$exe`" /Fo:`"$buildDir\\`""
        $tmpBat = Join-Path $buildDir "_build.bat"
        Set-Content -Path $tmpBat -Value $cmd -Encoding ASCII
        & cmd.exe /c $tmpBat
        if ($LASTEXITCODE -ne 0) { throw "MSVC build failed with exit code $LASTEXITCODE" }
        Write-Host "Built $exe"
        exit 0
    }
}
elseif ($cl) {
    $sources = (Get-ChildItem "$srcDir\*.c" | ForEach-Object { $_.FullName }) -join " "
    $exe = Join-Path $buildDir "minirdb.exe"
    & cl.exe /nologo /W3 /std:c17 /D_CRT_SECURE_NO_WARNINGS /Zi /I"$srcDir" $sources /Fe:"$exe" /Fo:"$buildDir\"
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed with exit code $LASTEXITCODE" }
    Write-Host "Built $exe"
    exit 0
}

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if ($gcc) {
    $sources = (Get-ChildItem "$srcDir\*.c" | ForEach-Object { $_.FullName }) -join " "
    $exe = Join-Path $buildDir "minirdb.exe"
    & gcc -std=c11 -O0 -g -Wall -I"$srcDir" $sources.Split(" ") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "gcc build failed with exit code $LASTEXITCODE" }
    Write-Host "Built $exe"
    exit 0
}

throw "No C compiler found. Install MSVC Build Tools (with the C++ workload) or gcc/MinGW and retry."
