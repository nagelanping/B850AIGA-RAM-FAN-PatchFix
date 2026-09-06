# install.ps1 — RAMFan Virtual_TEMP 补丁安装（测试签名公测版，自动请求 UAC 提权）
#
# 适用：MAXSUN MS-iCraft B850 AIGA（PCI DEV_790B + NCT chip id 0xd802）内存风扇
#       曲线重启失效修复。驱动读取 DIMM 温度并持续写入 NCT6796D Virtual_TEMP
#       （SIO 页 0x0c / reg 0x36），使 FAN5=MEM_FAN 按 BIOS 曲线运行。
#
# 一键流程（2026-09-06 机主定案）：
#   1) 自动开启 testsigning（bcdedit /set testsigning on）
#   2) 自动关闭内存完整性 HVCI（注册表 DeviceGuard）
#   3) 自动导入同目录 RAMFanTestSign.cer（Root/TrustedPublisher）
#   4) 以上任一改动都需要重启才生效：脚本提示重启，重启后再次运行本脚本完成安装。
#   Secure Boot 无法用脚本关闭（在 BIOS/UEFI），检测到 ON 时提示去 BIOS 关闭后重试。
#
# 本脚本不会修改：风扇曲线、温度源、BIOS、固件。安装期间请停止 HWiNFO、
# OpenHardwareMonitor、AIDA64 等会访问 SMBus/SIO 的监控工具（共享端口）。
#
# 用法（普通权限即可，自动请求 UAC）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
#   pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1   # PS7
#   重启后再次运行同一命令完成安装。
#   排障参数：-SkipCheck 跳过环境检测与自动配置（手动改好设置后用）。
# 回滚：powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
[CmdletBinding()]
param(
    # 跳过环境检测/自动配置（排障用：已手动配好环境）
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

# ---- 5. 产物存在性（脚本与产物同目录，即发布包目录）----
$sys = Join-Path $scriptDir 'ramfan.sys'
$svc = Join-Path $scriptDir 'ramfan-service.exe'
$cer = Join-Path $scriptDir 'RAMFanTestSign.cer'
if (-not (Test-Path $sys)) { Fail "缺少 ramfan.sys（应与本脚本同目录）" }
if (-not (Test-Path $svc)) { Fail "缺少 ramfan-service.exe（应与本脚本同目录）" }

$needsReboot = $false

if (-not $SkipCheck) {
    # ---- 1. Secure Boot 检测（脚本无法自动关闭，须去 BIOS）----
    $sb = $false
    try { $sb = Confirm-SecureBootUEFI } catch { $sb = $false }  # 非 UEFI 视同不限制
    if ($sb) {
        Fail 'Secure Boot = ON。测试签名驱动无法加载。请在 BIOS/UEFI 中关闭 Secure Boot 后重试（本脚本不能自动改 BIOS 设置）。'
    }
    Write-Host '  [OK]   Secure Boot = OFF'

    # ---- 2. testsigning：OFF 则自动开启（需重启生效）----
    $ts = & "$env:WINDIR\System32\bcdedit.exe" /enum '{current}' 2>&1
    if ($ts -match 'testsigning\s+Yes') {
        Write-Host '  [OK]   testsigning = ON'
    } else {
        Write-Host '  [..]   testsigning = OFF，正在自动开启（bcdedit /set testsigning on）...'
        & "$env:WINDIR\System32\bcdedit.exe" /set testsigning on 2>&1 | ForEach-Object { Write-Host "        $_" }
        if ($LASTEXITCODE -ne 0) { Fail "bcdedit /set testsigning on 失败 (exit $LASTEXITCODE)" }
        $needsReboot = $true
    }

    # ---- 3. 内存完整性 (HVCI)：运行中或配置为启用则自动关闭（注册表，需重启生效）----
    $hvciRunning = $false
    $dg = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
    if ($dg -and $null -ne $dg.SecurityServicesRunning) {
        $hvciRunning = @($dg.SecurityServicesRunning) -contains 2   # 2 = HVCI 运行中
    }
    $scKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
    $hvciConfigured = $false
    if (Test-Path $scKey) {
        $v = (Get-ItemProperty -Path $scKey -ErrorAction SilentlyContinue).Enabled
        $hvciConfigured = ($v -eq 1)
    }
    # 运行中或配置为启用都关：装完重启后不会因 HVCI 重新启用而加载失败；
    # 对已关闭且未配置的机器无副作用。
    if ($hvciRunning -or $hvciConfigured) {
        Write-Host '  [..]   内存完整性（HVCI）正在运行或配置为启用，正在自动关闭（注册表 DeviceGuard）...'
        $dgKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
        New-Item -Path $dgKey -Force | Out-Null
        New-ItemProperty -Path $dgKey -Name 'EnableVirtualizationBasedSecurity' -PropertyType DWord -Value 0 -Force | Out-Null
        New-Item -Path $scKey -Force | Out-Null
        New-ItemProperty -Path $scKey -Name 'Enabled' -PropertyType DWord -Value 0 -Force | Out-Null
        $needsReboot = $true
    }
    Write-Host '  [OK]   内存完整性 = OFF（或已配置为关闭，重启后生效）'

    # ---- 4. 测试证书：未在受信任存储则自动导入同目录 .cer ----
    $testCert = Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -like '*RAMFanTestSign*' } | Select-Object -First 1
    if (-not $testCert) {
        if (-not (Test-Path $cer)) {
            Fail "未找到测试证书 $cer，且系统未安装 RAMFanTestSign。请使用完整发布包。"
        }
        Write-Host '  [..]   未找到测试证书 RAMFanTestSign，正在导入 RAMFanTestSign.cer ...'
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        # 证书导入不要求重启；bcdedit/注册表改动才要求
    } else {
        Write-Host '  [OK]   测试证书 RAMFanTestSign 已安装'
    }

    # ---- 5. 监控进程守护（并发访问 SMBus/SIO 是已知风险）----
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match 'hwinfo|openhardwaremonitor|cpuz|aida64|librehardware' } |
        ForEach-Object { Fail "检测到会访问 SMBus/SIO 的进程 $($_.ProcessName)，请先停止后再安装。" }

    if ($needsReboot) {
        Write-Host ''
        Write-Host '========== 需要重启系统 ==========' -ForegroundColor Yellow
        Write-Host '已开启 testsigning / 关闭内存完整性，这些改动重启后才生效。'
        Write-Host ''
        Write-Host '【注意】重启后不会自动完成安装。' -ForegroundColor Red
        Write-Host '重启完成后，请【再次运行本脚本】才会安装驱动与服务：' -ForegroundColor Red
        Write-Host ''
        Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File '$($MyInvocation.MyCommand.Path)'"
        Write-Host ''
        Write-Host '（重启后再次运行：若环境已满足，脚本会直接安装，无需再改设置。）'
        exit 0
    }
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

# ---- 7. 安装内核驱动（AUTO_START：开机自启）----
$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
Copy-Item $sys $dstSys -Force
sc.exe create RAMFanPnP type= kernel start= auto binPath= $dstSys 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "sc create RAMFanPnP 失败 (exit $LASTEXITCODE)" }
sc.exe start RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }
if (-not (Wait-Running RAMFanPnP 10)) {
    sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }
    Fail '驱动未能启动（已删除服务项 RAMFanPnP，可修复后重跑）。常见原因：testsigning /set 后尚未重启、测试证书未导入本机、或 HVCI/Secure Boot 未按前置条件关闭。查看系统事件日志 7045/7000。'
}
Write-Host '  [OK] 内核驱动 RAMFanPnP RUNNING（AUTO_START）' -ForegroundColor Green

# ---- 8. 安装喂值服务（复制到稳定路径再注册；AUTO_START + 依赖 RAMFanPnP）----
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
Write-Host '  [OK] 喂值服务 RAMFan RUNNING（AUTO_START，开机自启）' -ForegroundColor Green

Write-Host ''
Write-Host '=== 安装完成 ===' -ForegroundColor Green
Write-Host '两个服务均为 AUTO_START：系统重启后自动恢复喂值，无需手动启动。'
Write-Host '喂值日志：C:\ProgramData\RAMFan\ramfan.log'
Write-Host "查看最近写入：Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20"
Write-Host "单次自检：& '$svcDst' --once"
Write-Host "卸载回滚：powershell -NoProfile -ExecutionPolicy Bypass -File '$scriptDir\uninstall.ps1'"
