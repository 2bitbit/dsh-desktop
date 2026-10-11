<#
.SYNOPSIS
    编译并运行隔离市场重启认证回归，不打开浏览器或真实 DSH。
.DESCRIPTION
    使用测试进程私有 TEMP 与自建 Node 子进程，产物仅位于仓库 build/tests。
    HTTP 夹具验证兑换合同，不等于真实 DSH 端到端测试。
#>
[CmdletBinding()]
param(
    [string]$NodePath,
    [string]$CompilerPath,
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$rootDir = (Get-Item $PSScriptRoot).Parent.FullName
$source = Join-Path $rootDir 'tests\restart_auth.cpp'
$fixture = Join-Path $rootDir 'tests\restart_fixture.js'
$outputDir = Join-Path $rootDir 'build\tests'
$output = Join-Path $outputDir 'restart_auth.exe'

if (-not $CompilerPath) {
    $compiler = Get-Command g++ -ErrorAction Stop
    $CompilerPath = $compiler.Source
}
if (-not $NodePath) {
    $node = Get-Command node -ErrorAction Stop
    $NodePath = $node.Source
}
foreach ($path in @($CompilerPath, $NodePath, $source, $fixture)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "缺少测试输入：$path"
    }
}
if (-not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

# pnpm 的命令入口可能为 shim；让 Node 报告真正可执行文件路径。
$resolvedNode = & $NodePath --input-type=module --eval 'console.log(process.execPath)'
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $resolvedNode -PathType Leaf)) {
    throw '无法解析 Node 实际可执行文件。'
}
$NodePath = $resolvedNode

Write-Host '检查隔离 HTTP 夹具语法…'
& $NodePath --check $fixture
if ($LASTEXITCODE -ne 0) { throw "夹具语法检查失败：$LASTEXITCODE" }

Write-Host '编译认证回归测试（警告视为错误）…'
$compileArgs = @(
    '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
    '-static', '-static-libgcc', '-static-libstdc++',
    $source, '-o', $output,
    '-lshlwapi', '-lshell32', '-lole32', '-luser32', '-lgdi32',
    '-liphlpapi', '-lws2_32', '-lwinhttp'
)
& $CompilerPath @compileArgs
if ($LASTEXITCODE -ne 0) { throw "回归测试编译失败：$LASTEXITCODE" }
if ($CompileOnly) {
    Write-Host '仅编译完成，未启动测试夹具。'
    exit 0
}

$runDir = Join-Path $outputDir ('run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir | Out-Null
Write-Host '运行隔离回归；不启动真实 DSH 或浏览器…'
& $output $NodePath $fixture $runDir
if ($LASTEXITCODE -ne 0) { throw "隔离回归失败：$LASTEXITCODE" }
Write-Host '隔离回归通过。'
