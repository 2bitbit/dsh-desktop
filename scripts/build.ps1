<#
.SYNOPSIS
    编译 DSH Desktop（C++ Win32 GUI 启动器）。
.DESCRIPTION
    1) 先用 windres / rc.exe 把 src/dsh_desktop.rc（内嵌托盘图标 src/tray_icon.ico）编译成资源对象。
    2) 再把 src/dsh_desktop.cpp 与资源一起编译为 bin/dsh_desktop.exe。
    支持 GCC、Clang、MSVC；入口为 wWinMain，需 Unicode / 无控制台子系统。
    已在 MinGW-w64 GCC 15 上实测；Clang / MSVC 两条路径未实测。
#>

[CmdletBinding()]
param(
    [ValidateSet("Auto", "GCC", "Clang", "MSVC")]
    [string]$Compiler = "Auto",
    [switch]$DebugBuild
)

$ErrorActionPreference = "Stop"
$rootDir = (Get-Item $PSScriptRoot).Parent.FullName
$srcDir = Join-Path $rootDir "src"
$srcFile = Join-Path $srcDir "dsh_desktop.cpp"
$rcFile = Join-Path $srcDir "dsh_desktop.rc"
$icoFile = Join-Path $srcDir "tray_icon.ico"
$binDir = Join-Path $rootDir "bin"
$outFile = Join-Path $binDir "dsh_desktop.exe"
$resFile = Join-Path $binDir "dsh_desktop.res"

# 托盘图标是关闭语义的一部分：资源缺失时启动器会在运行期禁用「叉号收进托盘」，
# 所以这里缺输入就直接失败，不留「编出来能跑但少功能」的产物。
foreach ($required in @($srcFile, $rcFile, $icoFile)) {
    if (-not (Test-Path $required)) {
        Write-Error "缺少构建输入: $required"
        exit 1
    }
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

$resourceCompiler = if ($resolved -eq "MSVC") { "rc.exe" } else { "windres" }
if (-not (Test-CommandAvailable $resourceCompiler)) {
    Write-Error "PATH 中未找到资源编译器 $resourceCompiler —— 托盘图标无法内嵌，构建中止。"
    exit 1
}

Write-Host "==> 使用 $resolved 编译 dsh_desktop ..." -ForegroundColor Cyan

$libs = @("-lshlwapi", "-lshell32", "-lole32", "-luser32", "-lgdi32", "-liphlpapi", "-lws2_32")
$optFlags = if ($DebugBuild) { @("-g", "-O0") } else { @("-O3", "-s") }

# ---------- 1. 资源：内嵌托盘图标 ----------
Write-Host "==> 编译资源 $rcFile" -ForegroundColor Cyan
if ($resolved -eq "MSVC") {
    $resArgs = @("/nologo", "/I", $srcDir, "/fo", $resFile, $rcFile)
    Write-Host "Command: rc.exe $($resArgs -join ' ')" -ForegroundColor DarkGray
    & rc.exe @resArgs
} else {
    # windres 以 .rc 所在目录解析 ICON 的相对路径，所以无需切工作目录。
    $resArgs = @("-c", "65001", $rcFile, "-O", "coff", "-o", $resFile)
    Write-Host "Command: windres $($resArgs -join ' ')" -ForegroundColor DarkGray
    & windres @resArgs
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $resFile)) {
    Write-Error "资源编译失败，退出码 $LASTEXITCODE"
    exit 1
}

# ---------- 2. 编译并链接 ----------
switch ($resolved) {
    "GCC" {
        $compileArgs = @(
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-municode",
            "-mwindows",
            "-static",
            "-static-libgcc",
            "-static-libstdc++"
        ) + $optFlags + @(
            $srcFile,
            $resFile,
            "-o", $outFile
        ) + $libs

        Write-Host "Command: g++ $($compileArgs -join ' ')" -ForegroundColor DarkGray
        & g++ @compileArgs
    }
    "Clang" {
        $compileArgs = @(
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-municode",
            "-mwindows",
            "-static",
            "-static-libgcc",
            "-static-libstdc++"
        ) + $optFlags + @(
            $srcFile,
            $resFile,
            "-o", $outFile
        ) + $libs

        Write-Host "Command: clang++ $($compileArgs -join ' ')" -ForegroundColor DarkGray
        & clang++ @compileArgs
    }
    "MSVC" {
        $clOpt = if ($DebugBuild) { @("/Od", "/Zi") } else { @("/O2", "/GL") }
        $compileArgs = @(
            "/nologo",
            "/std:c++17",
            "/W4",
            "/EHsc",
            "/DUNICODE",
            "/D_UNICODE"
        ) + $clOpt + @(
            $srcFile,
            $resFile,
            "/Fe:$outFile",
            "/link",
            "/SUBSYSTEM:WINDOWS",
            "shlwapi.lib",
            "shell32.lib",
            "ole32.lib",
            "user32.lib",
            "gdi32.lib",
            "iphlpapi.lib",
            "ws2_32.lib"
        )

        Write-Host "Command: cl.exe $($compileArgs -join ' ')" -ForegroundColor DarkGray
        & cl.exe @compileArgs
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
