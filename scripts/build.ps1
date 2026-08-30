<#
.SYNOPSIS
    Build script for DSH PWA Shim (C++ Win32 GUI).
.DESCRIPTION
    Compiles src/dsh_pwa_shim.cpp into bin/dsh_pwa_shim.exe using GCC, Clang, or MSVC.
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

if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

function Test-CommandAvailable {
    param([string]$cmd)
    return [bool](Get-Command $cmd -ErrorAction SilentlyContinue)
}

# Resolve compiler
$resolved = ""
if ($Compiler -eq "Auto") {
    if (Test-CommandAvailable "g++") { $resolved = "GCC" }
    elseif (Test-CommandAvailable "clang++") { $resolved = "Clang" }
    elseif (Test-CommandAvailable "cl.exe") { $resolved = "MSVC" }
    else {
        Write-Error "No supported C++ compiler found in PATH (g++, clang++, or cl.exe). Please install MinGW-w64, LLVM Clang, or Visual Studio C++ Build Tools."
        exit 1
    }
} else {
    $resolved = $Compiler
}

Write-Host "==> Compiling dsh_pwa_shim with $resolved..." -ForegroundColor Cyan

$libs = @("-lws2_32", "-lshlwapi", "-lshell32", "-lole32", "-luser32")
$optFlags = if ($DebugBuild) { @("-g", "-O0") } else { @("-O3", "-s") }

switch ($resolved) {
    "GCC" {
        $args = @(
            "-std=c++17",
            "-Wall",
            "-Wextra",
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
            "-mwindows"
        ) + $optFlags + @(
            $srcFile,
            "-o", $outFile
        ) + $libs
        
        Write-Host "Command: clang++ $($args -join ' ')" -ForegroundColor DarkGray
        & clang++ @args
    }
    "MSVC" {
        $clOpt = if ($DebugBuild) { "/Od /Zi" } else { "/O2 /GL" }
        $args = @(
            "/nologo",
            "/std:c++17",
            "/W4",
            "/EHsc"
        ) + ($clOpt -split " ") + @(
            $srcFile,
            "/Fe:$outFile",
            "/link",
            "/SUBSYSTEM:WINDOWS",
            "ws2_32.lib",
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
    Write-Host "[OK] Build succeeded: $outFile ($sizeKb KB)" -ForegroundColor Green
} else {
    Write-Error "Build failed with exit code $LASTEXITCODE"
    exit 1
}
