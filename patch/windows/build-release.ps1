# build-release.ps1 — 生成 Windows 测试签名发布包到 <仓库根>\release\windows\
#
# 所有路径基于本脚本位置推导（$PSScriptRoot → patch\windows → patch → 仓库根），
# 不依赖当前工作目录；可在任意目录运行。
#
# 前置：已运行 build.ps1 -Configuration Release 产出：
#   patch/windows/driver/x64/Release/ramfan.sys
#   patch/windows/service/x64/Release/ramfan-service.exe
#   测试证书 RAMFanTestSign 已在本机（experiment-b-prep.ps1 创建过）。
#
# 产物：release/windows/（该目录 gitignore，不入库）——平铺：
#   ramfan.sys / ramfan-service.exe / install.ps1 / uninstall.ps1 /
#   INSTALL.md / RAMFanTestSign.cer
#
# 用法：pwsh -NoProfile -File .\patch\windows\build-release.ps1

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# ---- 0. 路径解析（全部基于脚本位置，不依赖 cwd）----
$patchDir = $PSScriptRoot                 # patch\windows
$repoRoot = Split-Path (Split-Path $patchDir -Parent) -Parent   # 仓库根
$outDir = Join-Path $repoRoot 'release\windows'

Write-Host "仓库根: $repoRoot"
Write-Host "发布目录: $outDir"

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# ---- 1. 定位 signtool 与测试证书 ----
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$signtool = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { Fail '未找到 signtool。' }
$certName = 'RAMFanTestSign'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like "*CN=$certName*" } | Select-Object -First 1
if (-not $cert) { Fail "未找到测试证书 $certName；先运行 experiment-b-prep.ps1 创建。" }

# ---- 2. 源产物（patch\windows 下）----
$sys = Join-Path $patchDir 'driver\x64\Release\ramfan.sys'
$svc = Join-Path $patchDir 'service\x64\Release\ramfan-service.exe'
if (-not (Test-Path $sys)) { Fail "未找到驱动: $sys（先运行 build.ps1 -Configuration Release）" }
if (-not (Test-Path $svc)) { Fail "未找到服务: $svc（先运行 build.ps1 -Configuration Release）" }

# ---- 3. 输出目录 ----
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# ---- 4. 签名（内核驱动需 /ph 页哈希，实测缺 /ph 报错 577）----
Write-Host ''
Write-Host '签名 ramfan.sys（/ph）...'
& $signtool.FullName sign /v /fd sha256 /ph /sm /s My /n $certName $sys 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "驱动签名失败 (exit $LASTEXITCODE)" }
Write-Host '签名 ramfan-service.exe ...'
& $signtool.FullName sign /v /fd sha256 /sm /s My /n $certName $svc 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "服务签名失败 (exit $LASTEXITCODE)" }

# ---- 5. 组装（复制到发布目录；签名后的 .sys 复制自 Release 产物路径）----
$signedSys = $sys      # signtool 原位签名 Release 产物，直接复制即可
Copy-Item $signedSys (Join-Path $outDir 'ramfan.sys') -Force
Copy-Item $svc (Join-Path $outDir 'ramfan-service.exe') -Force
foreach ($f in 'install.ps1', 'uninstall.ps1', 'INSTALL.md', 'RAMFanTestSign.cer') {
    $src = Join-Path $patchDir $f
    if (-not (Test-Path $src)) { Fail "缺少分发文件 $src" }
    Copy-Item $src (Join-Path $outDir $f) -Force
}

Write-Host ''
Write-Host '=== 发布包已生成 ===' -ForegroundColor Green
Write-Host "目录：$outDir"
Get-ChildItem $outDir | Select-Object Name, Length
Write-Host ''
Write-Host '签名验证：'
& $signtool.FullName verify /pa (Join-Path $outDir 'ramfan.sys') 2>&1 | Select-Object -Last 2
