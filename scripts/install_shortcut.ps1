<#
.SYNOPSIS
    为 DSH Desktop Shim 创建或更新 Windows 快捷方式。
.DESCRIPTION
    在开始菜单和/或桌面创建指向 dsh_pwa_shim.exe 的快捷方式。
    点击快捷方式等效于在指定工作目录执行 `dsh web --no-open`，
    并由 shim 解析本次进程的认证 URL 后拉起 Edge --app 窗口。
    无需预先安装 Edge PWA。
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

if (-not $PSBoundParameters.ContainsKey('TargetDirectory') -or [string]::IsNullOrWhiteSpace($TargetDirectory)) {
    if (-not $NonInteractive) {
        Add-Type -AssemblyName System.Windows.Forms
        $folderDialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $folderDialog.Description = "请选择工作目录（快捷方式将等效于在此目录执行 'dsh web --no-open'）:"
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

$TargetDirectory = (Resolve-Path $TargetDirectory).Path

if (-not $PSBoundParameters.ContainsKey('ShortcutName') -or [string]::IsNullOrWhiteSpace($ShortcutName)) {
    $folderLeaf = Split-Path $TargetDirectory -Leaf
    if ($folderLeaf -and $TargetDirectory -ne $HOME) {
        $ShortcutName = "DSH ($folderLeaf)"
    } else {
        $ShortcutName = "DeepSeek Harness"
    }
}

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
            Write-Host "未找到可执行文件，正在先运行 build.ps1 ..." -ForegroundColor Yellow
            & $buildScript
            $ExePath = $repoBinExe
        } else {
            Write-Error "未在 $scriptDir 或 $rootDir\bin 找到 dsh_pwa_shim.exe"
            exit 1
        }
    }
}

if (-not (Test-Path $ExePath)) {
    Write-Error "目标程序不存在: $ExePath"
    exit 1
}

$WshShell = New-Object -ComObject WScript.Shell

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
Write-Host " [等效命令] : 在该目录执行 'dsh web --no-open'，解析 token 后拉起 Edge --app" -ForegroundColor Green
Write-Host " [目标程序] : $ExePath" -ForegroundColor DarkGray
Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray

foreach ($dest in $destinations) {
    $shortcut = $WshShell.CreateShortcut($dest)
    $shortcut.TargetPath = $ExePath
    $shortcut.Arguments = "--cwd `"$TargetDirectory`""
    $shortcut.WorkingDirectory = $TargetDirectory
    $shortcut.Description = "Equivalent to running 'dsh web --no-open' in '$TargetDirectory', then opening Edge --app with the process token URL."
    $shortcut.IconLocation = "$ExePath,0"
    $shortcut.Save()
    Write-Host "[OK] 已生成快捷方式: $dest" -ForegroundColor Green
}
Write-Host "============================================================" -ForegroundColor Cyan
