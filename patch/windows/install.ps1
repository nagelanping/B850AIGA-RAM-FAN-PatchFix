# install.ps1 — RAMFan Virtual_TEMP 补丁安装（测试签名版，自动请求 UAC 提权）
#
# 适用：MAXSUN MS-iCraft B850 AIGA（PCI DEV_790B + NCT chip id 0xd802）内存风扇
#       曲线重启失效修复。驱动读取 DIMM 温度并持续写入 NCT6796D Virtual_TEMP
#       （SIO 页 0x0c / reg 0x36），使 FAN5=MEM_FAN 按 BIOS 曲线运行。
#
# 重要前置条件（本脚本只检测并提示，由用户自行判断与关闭，绝不自动修改
# 系统安全设置）：
#   1) Secure Boot 必须为 OFF —— 测试签名驱动无法在 Secure Boot 下加载。
#      关闭：BIOS/UEFI 设置 → Secure Boot → Disabled（各主板菜单位置不同）。
#   2) testsigning 必须 ON：
#      管理员终端执行：bcdedit /set testsigning on  → 重启。
#   3) 内存完整性（Memory Integrity / HVCI，内核隔离）必须 OFF —— 开启时会
#      阻止加载测试签名驱动。Windows 安全中心 → 设备安全性 → 内核隔离 →
#      内存完整性 → 关闭 → 重启。
#   4) 不修改风扇曲线/温度源；不写 BIOS；不刷固件。
#   5) 安装/运行期间请停止 HWiNFO、OpenHardwareMonitor、AIDA64 等会访问
#      SMBus/SIO 端口的监控工具（本补丁与它们共享端口）。
#
# 用法（普通权限即可，自动请求 UAC）：powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
#   （装有 PowerShell 7 可用：pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1）
# 回滚：powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
[CmdletBinding()]
param(
    # 跳过环境检测（排障用）
    [switch]$SkipCheck
)
$ErrorActionPreference = 'Stop'
$scriptDir = $PSScriptRoot

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Wait-Running([string]$name, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    do {
        $s = sc.exe query $name 2>&1
        if ($s -match 'RUNNING') { return $true }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    return $false
}

# ---- 0. 提权：非管理员时以当前宿主自动重启（UAC），不写死 pwsh.exe ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $hostExe = (Get-Process -Id $PID -ErrorAction SilentlyContinue).Path
    if (-not $hostExe) {
        $hostExe = Join-Path $PSHOME $(if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' })
    }
    if (-not (Test-Path -LiteralPath $hostExe)) {
        Write-Host "ERROR: 找不到 PowerShell 宿主: $hostExe" -ForegroundColor Red
        exit 1
    }
    Write-Host '需要管理员权限，正在请求提升（UAC）...'
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SkipCheck) { $args2 += '-SkipCheck' }
    try {
        $p = Start-Process -FilePath $hostExe -ArgumentList $args2 -Verb RunAs -Wait -PassThru
    } catch {
        Write-Host "ERROR: 请求管理员权限失败或已被取消：$($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
    exit $p.ExitCode
}

if (-not $SkipCheck) {
    # ---- 1. Secure Boot 检测 ----
    $sb = $false
    try { $sb = Confirm-SecureBootUEFI } catch { $sb = $false }  # 非 UEFI 视同不限制
    if ($sb) {
        Write-Host '  [FAIL] Secure Boot = ON。测试签名驱动无法加载。' -ForegroundColor Red
        Write-Host '        请在 BIOS/UEFI 中关闭 Secure Boot 后重试（测试版前提，由你自行决定）。'
        exit 1
    }
    Write-Host '  [OK]   Secure Boot = OFF'

    # ---- 2. testsigning 检测 ----
    $ts = & "$env:WINDIR\System32\bcdedit.exe" /enum '{current}' 2>&1
    if ($ts -match 'testsigning\s+Yes') {
        Write-Host '  [OK]   testsigning = ON'
    } else {
        Write-Host '  [FAIL] testsigning = OFF。' -ForegroundColor Red
        Write-Host '        管理员终端执行：bcdedit /set testsigning on，然后重启。'
        exit 1
    }

    # ---- 3. 内存完整性 (HVCI) 检测：以实际运行态为准（WMI），注册表配置仅回退 ----
    # 注意：用户可能曾开过 HVCI 后因关 Secure Boot 而无法运行，注册表 Enabled 残留
    # 1 但实际未运行——此时不应误报。运行态才是能否加载测试签名驱动的判据。
    $hvciOn = $false
    $dg = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
    if ($dg -and $null -ne $dg.SecurityServicesRunning) {
        $hvciOn = @($dg.SecurityServicesRunning) -contains 2   # 2 = HVCI 运行中
    } else {
        $scKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
        if (Test-Path $scKey) {
            $v = (Get-ItemProperty -Path $scKey -ErrorAction SilentlyContinue).Enabled
            $hvciOn = ($v -eq 1)   # 拿不到 WMI 时按配置回退
        }
    }
    if ($hvciOn) {
        Write-Host '  [FAIL] 内存完整性（Memory Integrity / HVCI）正在运行，会阻止测试签名驱动。' -ForegroundColor Red
        Write-Host '        关闭：Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 → 关 → 重启。'
        Write-Host '        （若注册表残留但实际未运行，以 Windows 安全中心显示为准，可加 -SkipCheck 复核。）'
        exit 1
    }
    Write-Host '  [OK]   内存完整性 = OFF'

    # ---- 4. 监控进程守护（并发访问 SMBus/SIO 是已知风险）----
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match 'hwinfo|openhardwaremonitor|cpuz|aida64|librehardware' } |
        ForEach-Object { Fail "检测到会访问 SMBus/SIO 的进程 $($_.ProcessName)，请先停止后再安装。" }
}

# ---- 5. 产物存在性（脚本与产物同目录，即 patch/windows/）----
$sys = Join-Path $scriptDir 'ramfan.sys'
$svc = Join-Path $scriptDir 'ramfan-service.exe'
if (-not (Test-Path $sys)) { Fail "缺少 ramfan.sys（应与本脚本同目录）" }
if (-not (Test-Path $svc)) { Fail "缺少 ramfan-service.exe（应与本脚本同目录）" }

# ---- 5.1 测试证书软校验（缺失仅提示，不阻断——受控实验可能已装）----
$testCert = Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -like '*RAMFanTestSign*' } | Select-Object -First 1
if (-not $testCert) {
    Write-Host 'WARN: 未在受信任存储找到测试证书 RAMFanTestSign。' -ForegroundColor Yellow
    Write-Host '      若驱动加载失败（错误 577），需先导入测试证书；受控实验仅限目标机。'
}

# ---- 6. 冲突服务检测 ----
$existing = sc.exe query RAMFanPnP 2>&1
if ($existing -match 'SERVICE_NAME: RAMFanPnP') {
    Fail '内核驱动服务 RAMFanPnP 已存在。先运行 uninstall.ps1 清理，或直接 sc start RAMFanPnP 启动。'
}
$svcExisting = sc.exe query RAMFan 2>&1
if ($svcExisting -match 'SERVICE_NAME: RAMFan') {
    Fail '喂值服务 RAMFan 已存在。先运行 uninstall.ps1 清理。'
}

Write-Host ''
Write-Host '环境检测通过，开始安装 ...' -ForegroundColor Green

# ---- 7. 安装内核驱动 ----
$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
Copy-Item $sys $dstSys -Force
sc.exe create RAMFanPnP type= kernel start= demand binPath= $dstSys 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "sc create RAMFanPnP 失败 (exit $LASTEXITCODE)" }
sc.exe start RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }
if (-not (Wait-Running RAMFanPnP 10)) {
    sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }
    Fail '驱动未能启动（已删除服务项 RAMFanPnP，可修复后重跑）。常见原因：testsigning /set 后尚未重启、测试证书未导入本机、或 HVCI/Secure Boot 未按前置条件关闭。查看系统事件日志 7045/7000。'
}
Write-Host '  [OK] 内核驱动 RAMFanPnP RUNNING' -ForegroundColor Green

# ---- 8. 安装喂值服务（复制到稳定路径再注册；DEMAND_START，验收后才改自动启动）----
$progData = 'C:\ProgramData\RAMFan'
New-Item -ItemType Directory -Path $progData -Force | Out-Null
$svcDst = Join-Path $progData 'ramfan-service.exe'
Copy-Item $svc $svcDst -Force
& $svcDst --install
if ($LASTEXITCODE -ne 0) { Fail '用户态服务安装失败。' }
sc.exe start RAMFan 2>&1 | ForEach-Object { Write-Host "  $_" }
if (-not (Wait-Running RAMFan 15)) {
    sc.exe stop RAMFan 2>&1 | ForEach-Object { Write-Host "  $_" }
    sc.exe delete RAMFan 2>&1 | ForEach-Object { Write-Host "  $_" }
    Fail '喂值服务未能进入 RUNNING（已删除服务 RAMFan）。查看日志 C:\ProgramData\RAMFan\ramfan.log。'
}
Write-Host '  [OK] 喂值服务 RAMFan RUNNING' -ForegroundColor Green

Write-Host ''
Write-Host '=== 安装完成 ===' -ForegroundColor Green
Write-Host '喂值日志：C:\ProgramData\RAMFan\ramfan.log'
Write-Host "查看最近写入：Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20"
Write-Host "单次自检：& '$svcDst' --once"
Write-Host "卸载回滚：powershell -NoProfile -ExecutionPolicy Bypass -File '$scriptDir\uninstall.ps1'"
Write-Host ''
Write-Host '注意：当前服务为 DEMAND_START（手动启动）。如需开机自启，待验收通过后另行处理。'
