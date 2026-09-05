# identity-gate-prep.ps1 — 身份门禁阶段准备与实机只读验证（管理员，机主执行）
# 模型：非 PnP KMDF 控制设备驱动（2026-09-05 机主批准）。无 INF、无 PnP 绑定。
# 流程：检查环境 → 用测试证书签名 ramfan.sys（/ph 页哈希）→ 复制到
#       %WinDir%\System32\drivers → sc create RAMFanPnP (type= kernel, demand)
#       → sc start → 运行 ramfan-service.exe --identity 只读身份门禁 → 输出结果。
# 不做：SMBus 事务、NCT 写、驱动不加载时的服务启动、testsigning 关闭。
# 回滚：identity-gate-rollback.ps1。
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipIdentityRun   # 只签名+加载，不运行 --identity（供调试）
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$log = Join-Path $root 'experiment-b-logs'
New-Item -ItemType Directory -Path $log -Force | Out-Null
$logFile = Join-Path $log 'identity-gate.log'
Start-Transcript -Path $logFile -Force

function Fail([string]$msg) {
    Stop-Transcript | Out-Null
    throw $msg
}

# ---- 1. 环境检查 ----
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail '需要管理员权限。'
}
if (Confirm-SecureBootUEFI) {
    Fail 'Secure Boot 为 True：测试签名驱动不能加载。本机当前为 False；若已变更必须先关闭 Secure Boot。'
}
$ts = & "$env:WINDIR\System32\bcdedit.exe" /enum '{current}' 2>&1
if (-not ($ts | Select-String 'testsigning\s+Yes' -Quiet)) {
    Fail 'testsigning 未开启。先执行 experiment-b-prep.ps1 或手动 bcdedit /set testsigning on 并重启。'
}
Get-Process | Where-Object { $_.ProcessName -match 'hwinfo|openhardwaremonitor|cpuz|aida64|librehardware' } |
    ForEach-Object { Fail "检测到会访问端口的进程 $($_.ProcessName)，请先停止。" }

$sys = Join-Path $root "driver\x64\$Configuration\ramfan.sys"
$svc = Join-Path $root "service\x64\$Configuration\ramfan-service.exe"
if (-not (Test-Path $sys)) { Fail "未找到驱动: $sys（先运行 build.ps1 -Configuration $Configuration）" }
if (-not (Test-Path $svc)) { Fail "未找到服务程序: $svc（先运行 build.ps1 -Configuration $Configuration）" }

# ---- 2. 测试证书 ----
$certName = 'RAMFanTestSign'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like "*CN=$certName*" } | Select-Object -First 1
if (-not $cert) { Fail "未找到测试证书 $certName；先执行 experiment-b-prep.ps1 创建。" }

# ---- 3. 签名（内核驱动需要 /ph 页哈希，实测缺 /ph 报错 577）----
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$signtool = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { Fail '未找到 signtool。' }
$sysSigned = Join-Path $log 'ramfan-identity-gate.sys'
Copy-Item $sys $sysSigned -Force
& $signtool.FullName sign /v /fd sha256 /ph /sm /s My /n $certName $sysSigned 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "签名失败 (exit $LASTEXITCODE)" }

# ---- 4. 复制到 System32\drivers 并创建内核服务 ----
$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
Copy-Item $sysSigned $dstSys -Force
"已复制驱动到 $dstSys"

$existing = sc.exe query RAMFanPnP 2>&1
if ($existing -match 'SERVICE_NAME: RAMFanPnP') {
    Fail '服务 RAMFanPnP 已存在（上次未回滚？）。先运行 identity-gate-rollback.ps1。'
}
sc.exe create RAMFanPnP type= kernel start= demand binPath= $dstSys 2>&1 | ForEach-Object { "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "sc create 失败 (exit $LASTEXITCODE)" }

# ---- 5. 启动驱动 ----
sc.exe start RAMFanPnP 2>&1 | ForEach-Object { "  $_" }
Start-Sleep -Milliseconds 800
$q = sc.exe query RAMFanPnP 2>&1
if (-not ($q -match 'RUNNING')) {
    sc.exe delete RAMFanPnP 2>&1 | Out-Null
    Fail "驱动未能启动（sc query 非 RUNNING）。查看 system 事件日志 7045/7000。"
}
'驱动 RAMFanPnP 已启动（RUNNING）。'

# ---- 6. 运行只读身份门禁 ----
if (-not $SkipIdentityRun) {
    "运行 ramfan-service.exe --identity ..."
    & $svc --identity
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        "身份门禁未通过（exit $code）。驱动保持加载供排查；完成后运行 identity-gate-rollback.ps1。"
        Stop-Transcript | Out-Null
        exit 1
    }
}

Stop-Transcript | Out-Null
Write-Host ""
Write-Host "=== 身份门禁完成 ==="
Write-Host "驱动已加载：RAMFanPnP（内核服务）"
Write-Host "身份检查结果与日志：$logFile"
Write-Host ""
Write-Host "验证后清理："
Write-Host "  pwsh -File .\patch\windows\identity-gate-rollback.ps1"
