<#
.SYNOPSIS
    编译 DSH Desktop Shim（C++ Win32 GUI）。
.DESCRIPTION
    将 src/dsh_pwa_shim.cpp 编译为 bin/dsh_pwa_shim.exe。
    支持 GCC、Clang、MSVC；入口为 wWinMain，需 Unicode / 无控制台子系统。
#>

[CmdletBinding()]
param(
    [ValidateSet("Auto", "GCC", "Clang", "MSVC")]
    [string]$Compiler = "Auto",
    [switch]$DebugBuild
)

$ErrorActionPreference = "Stop"
$rootDir = (Get-Item $PSScriptRoot).Parent.FullName
$srcFile = Join-Path $rootDir "src\dsh_pwa_shim.cpp"
$binDir = Join-Path $rootDir "bin"
$outFile = Join-Path $binDir "dsh_pwa_shim.exe"

if (-not (Test-Path $srcFile)) {
    Write-Error "源文件不存在: $srcFile"
    exit 1
}

if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

function Test-CommandAvailable {
    param([string]$cmd)
    return [bool](Get-Command $cmd -ErrorAction SilentlyContinue)
}

$resolved = ""
if ($Compiler -eq "Auto") {
    if (Test-CommandAvailable "g++") { $resolved = "GCC" }
    elseif (Test-CommandAvailable "clang++") { $resolved = "Clang" }
    elseif (Test-CommandAvailable "cl.exe") { $resolved = "MSVC" }
    else {
        Write-Error "PATH 中未找到 C++ 编译器 (g++ / clang++ / cl.exe)。请安装 MinGW-w64、LLVM Clang 或 Visual Studio C++ Build Tools。"
        exit 1
    }
} else {
    $resolved = $Compiler
}

Write-Host "==> 使用 $resolved 编译 dsh_pwa_shim ..." -ForegroundColor Cyan

$libs = @("-lshlwapi", "-lshell32", "-lole32", "-luser32")
$optFlags = if ($DebugBuild) { @("-g", "-O0") } else { @("-O3", "-s") }

switch ($resolved) {
    "GCC" {
        $args = @(
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-municode",
            "-mwindows"
        ) + $optFlags + @(
            $srcFile,
            "-o", $outFile
        ) + $libs

        Write-Host "Command: g++ $($args -join ' ')" -ForegroundColor DarkGray
        & g++ @args
    }
    "Clang" {
        $args = @(
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-municode",
            "-mwindows"
        ) + $optFlags + @(
            $srcFile,
            "-o", $outFile
        ) + $libs

        Write-Host "Command: clang++ $($args -join ' ')" -ForegroundColor DarkGray
        & clang++ @args
    }
    "MSVC" {
        $clOpt = if ($DebugBuild) { @("/Od", "/Zi") } else { @("/O2", "/GL") }
        $args = @(
            "/nologo",
            "/std:c++17",
            "/W4",
            "/EHsc",
            "/DUNICODE",
            "/D_UNICODE"
        ) + $clOpt + @(
            $srcFile,
            "/Fe:$outFile",
            "/link",
            "/SUBSYSTEM:WINDOWS",
            "shlwapi.lib",
            "shell32.lib",
            "ole32.lib",
            "user32.lib"
        )

        Write-Host "Command: cl.exe $($args -join ' ')" -ForegroundColor DarkGray
        & cl.exe @args
    }
}

if ($LASTEXITCODE -eq 0 -and (Test-Path $outFile)) {
    $size = (Get-Item $outFile).Length
    $sizeKb = [Math]::Round($size / 1024, 2)
    Write-Host "[OK] 编译成功: $outFile ($sizeKb KB)" -ForegroundColor Green
} else {
    Write-Error "编译失败，退出码 $LASTEXITCODE"
    exit 1
}
