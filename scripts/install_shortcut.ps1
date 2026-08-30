<#
.SYNOPSIS
    Create or update Windows Shortcuts for DSH PWA Shim.
.DESCRIPTION
    Creates a shortcut in the Start Menu and/or Desktop pointing to dsh_pwa_shim.exe
    with a configured working directory (--cwd).
#>

[CmdletBinding()]
param(
    [string]$TargetDirectory = "$HOME",
    [string]$ShortcutName = "DSH (Silent PWA)",
    [ValidateSet("StartMenu", "Desktop", "Both")]
    [string]$Location = "Both",
    [string]$ExePath = ""
)

$ErrorActionPreference = "Stop"
$rootDir = (Get-Item $PSScriptRoot).Parent.FullName

if (-not $ExePath) {
    $ExePath = Join-Path $rootDir "bin\dsh_pwa_shim.exe"
}

if (-not (Test-Path $ExePath)) {
    Write-Host "Executable not found at $ExePath. Running build.ps1 first..." -ForegroundColor Yellow
    & "$PSScriptRoot\build.ps1"
}

$WshShell = New-Object -ComObject WScript.Shell

# Attempt to locate DeepSeek Harness PWA Icon
$pwaAppId = "hgiemfgfjhalibdoboikeiepnnjapnpc"
$edgeIconPattern = Join-Path $env:LOCALAPPDATA "Microsoft\Edge\User Data\Default\Web Applications\_crx__$pwaAppId\*.ico"
$pwaIcon = Get-ChildItem -Path $edgeIconPattern -ErrorAction SilentlyContinue | Select-Object -First 1

$destinations = @()
if ($Location -in @("StartMenu", "Both")) {
    $startMenuDir = [Environment]::GetFolderPath("Programs")
    $destinations += (Join-Path $startMenuDir "$ShortcutName.lnk")
}
if ($Location -in @("Desktop", "Both")) {
    $desktopDir = [Environment]::GetFolderPath("Desktop")
    $destinations += (Join-Path $desktopDir "$ShortcutName.lnk")
}

foreach ($dest in $destinations) {
    $shortcut = $WshShell.CreateShortcut($dest)
    $shortcut.TargetPath = $ExePath
    $shortcut.Arguments = "--cwd `"$TargetDirectory`""
    $shortcut.WorkingDirectory = $TargetDirectory
    $shortcut.Description = "Launch DeepSeek Harness (DSH) in silent PWA mode with automatic process lifecycle management."
    
    if ($pwaIcon) {
        $shortcut.IconLocation = "$($pwaIcon.FullName),0"
    } else {
        $shortcut.IconLocation = "$ExePath,0"
    }
    
    $shortcut.Save()
    Write-Host "[OK] Created shortcut: $dest" -ForegroundColor Green
    Write-Host "     -> Target: $ExePath" -ForegroundColor DarkGray
    Write-Host "     -> Working Directory (--cwd): $TargetDirectory" -ForegroundColor DarkGray
}
