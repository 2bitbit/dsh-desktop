<#
.SYNOPSIS
    Create or update Windows Shortcuts for DSH PWA Shim.
.DESCRIPTION
    Creates a shortcut in the Start Menu and/or Desktop pointing to dsh_pwa_shim.exe.
    The created shortcut is equivalent to running "dsh web --no-open" in the specified target directory.
#>

[CmdletBinding()]
param(
    [string]$TargetDirectory,
    [string]$ShortcutName,
    [ValidateSet("StartMenu", "Desktop", "Both")]
    [string]$Location = "Both",
    [string]$ExePath = "",
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
$scriptDir = $PSScriptRoot
$rootDir = (Get-Item $scriptDir).Parent.FullName

# 1. Resolve target working directory (CLI arg -> Interactive Folder Picker -> Fallback $HOME)
if (-not $PSBoundParameters.ContainsKey('TargetDirectory') -or [string]::IsNullOrWhiteSpace($TargetDirectory)) {
    if (-not $NonInteractive) {
        Add-Type -AssemblyName System.Windows.Forms
        $folderDialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $folderDialog.Description = "请选择工作目录 (快捷方式将等效于在此目录下执行 'dsh web --no-open'):"
        $folderDialog.ShowNewFolderButton = $true
        $folderDialog.SelectedPath = (Get-Location).Path
        
        $dialogResult = $folderDialog.ShowDialog()
        if ($dialogResult -eq [System.Windows.Forms.DialogResult]::OK -and -not [string]::IsNullOrWhiteSpace($folderDialog.SelectedPath)) {
            $TargetDirectory = $folderDialog.SelectedPath
        } else {
            $TargetDirectory = (Get-Location).Path
        }
    } else {
        $TargetDirectory = $HOME
    }
}

# Resolve full absolute path
$TargetDirectory = (Resolve-Path $TargetDirectory).Path

# 2. Resolve shortcut name
if (-not $PSBoundParameters.ContainsKey('ShortcutName') -or [string]::IsNullOrWhiteSpace($ShortcutName)) {
    $folderLeaf = Split-Path $TargetDirectory -Leaf
    if ($folderLeaf -and $TargetDirectory -ne $HOME) {
        $ShortcutName = "DSH ($folderLeaf)"
    } else {
        $ShortcutName = "DSH (Silent PWA)"
    }
}

# 3. Locate executable
if (-not $ExePath) {
    $siblingExe = Join-Path $scriptDir "dsh_pwa_shim.exe"
    $repoBinExe = Join-Path $rootDir "bin\dsh_pwa_shim.exe"
    
    if (Test-Path $siblingExe) {
        $ExePath = $siblingExe
    } elseif (Test-Path $repoBinExe) {
        $ExePath = $repoBinExe
    } else {
        $buildScript = Join-Path $scriptDir "build.ps1"
        if (Test-Path $buildScript) {
            Write-Host "Executable not found. Running build.ps1 first..." -ForegroundColor Yellow
            & $buildScript
            $ExePath = $repoBinExe
        } else {
            Write-Error "Could not find dsh_pwa_shim.exe in $scriptDir or $rootDir\bin"
            exit 1
        }
    }
}

$WshShell = New-Object -ComObject WScript.Shell

# 4. Attempt to locate DeepSeek Harness PWA Icon
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

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " 创建 DSH 桌面快捷方式" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " [工作目录] : $TargetDirectory" -ForegroundColor White
Write-Host " [等效命令] : 在该目录下执行 'dsh web --no-open' 并静默联动 PWA" -ForegroundColor Green
Write-Host " [目标程序] : $ExePath" -ForegroundColor DarkGray
Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray

foreach ($dest in $destinations) {
    $shortcut = $WshShell.CreateShortcut($dest)
    $shortcut.TargetPath = $ExePath
    $shortcut.Arguments = "--cwd `"$TargetDirectory`""
    $shortcut.WorkingDirectory = $TargetDirectory
    $shortcut.Description = "Equivalent to running 'dsh web --no-open' in '$TargetDirectory' with silent PWA lifecycle management."
    
    if ($pwaIcon) {
        $shortcut.IconLocation = "$($pwaIcon.FullName),0"
    } else {
        $shortcut.IconLocation = "$ExePath,0"
    }
    
    $shortcut.Save()
    Write-Host "[OK] 已生成快捷方式: $dest" -ForegroundColor Green
}
Write-Host "============================================================" -ForegroundColor Cyan
