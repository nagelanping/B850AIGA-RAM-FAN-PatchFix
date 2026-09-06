# build-release.ps1 — 签名并组装 Windows 公测版发布目录到 <仓库根>\release\windows\
#
# 本脚本只做：签名 + 复制发布文件到 release\windows\，不负责压缩打包。
# 前置：已运行 build.ps1 -Configuration Release；测试证书 RAMFanTestSign 已在本机。
#
# 用法：pwsh -NoProfile -File .\build-release.ps1
#       或  powershell -NoProfile -ExecutionPolicy Bypass -File .\build-release.ps1

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# ---- 0. 路径（基于脚本位置，不依赖 cwd）----
$patchDir = $PSScriptRoot
$repoRoot = Split-Path (Split-Path $patchDir -Parent) -Parent
$outDir = Join-Path $repoRoot 'release\windows'

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# ---- 1. 定位 signtool 与测试证书 ----
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$signtool = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { Fail '未找到 signtool。' }
$cert = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -like '*CN=RAMFanTestSign*' } | Select-Object -First 1
if (-not $cert) { Fail '未找到测试证书 RAMFanTestSign（先运行 experiment-b-prep.ps1 创建）。' }
$certName = $cert.Subject -replace '^CN=([^,]+).*$', '$1'

# ---- 2. 源产物 ----
$sys = Join-Path $patchDir 'driver\x64\Release\ramfan.sys'
$svc = Join-Path $patchDir 'service\x64\Release\ramfan-service.exe'
if (-not (Test-Path $sys)) { Fail '未找到驱动，先运行 build.ps1 -Configuration Release' }
if (-not (Test-Path $svc)) { Fail '未找到服务，先运行 build.ps1 -Configuration Release' }

# ---- 3. 输出目录 ----
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# ---- 4. 复制（源保持未签，签名对象是副本，重跑不累积签名）----
Copy-Item $sys (Join-Path $outDir 'ramfan.sys') -Force
Copy-Item $svc (Join-Path $outDir 'ramfan-service.exe') -Force
foreach ($f in 'install.ps1', 'uninstall.ps1', 'INSTALL.md', 'RAMFanTestSign.cer') {
    $src = Join-Path $patchDir $f
    if (-not (Test-Path $src)) { Fail "缺少分发文件 $src" }
    Copy-Item $src (Join-Path $outDir $f) -Force
}
foreach ($f in 'README.md', 'LICENSE') {
    $src = Join-Path $repoRoot $f
    if (-not (Test-Path $src)) { Fail "缺少包内文档 $src" }
    Copy-Item $src (Join-Path $outDir $f) -Force
}

# ---- 5. 签名发布副本 ----
$outSys = Join-Path $outDir 'ramfan.sys'
$outSvc = Join-Path $outDir 'ramfan-service.exe'
Write-Host '签名 ramfan.sys（/ph）...'
& $signtool.FullName sign /v /fd sha256 /ph /sm /s My /n $certName $outSys 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail '驱动签名失败' }
Write-Host '签名 ramfan-service.exe ...'
& $signtool.FullName sign /v /fd sha256 /sm /s My /n $certName $outSvc 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail '服务签名失败' }

# ---- 6. 验证 ----
& $signtool.FullName verify /pa $outSys 2>&1 | Select-Object -Last 1
& $signtool.FullName verify /pa $outSvc 2>&1 | Select-Object -Last 1

# ---- 7. 提示打包 ----
Write-Host ''
Write-Host '=== 发布目录已就绪 ===' -ForegroundColor Green
Write-Host "目录：$outDir"
Write-Host ''
Write-Host '请手动打包为（7-Zip）：'
Write-Host '  B850AIGA-RAM-FAN-PatchFix_Windows_TestSign.7z'
Write-Host '将 release\windows\ 下的全部文件压入该 7z，再上传 GitHub Releases。'
