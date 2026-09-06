# uninstall.ps1 — RAMFan Virtual_TEMP 补丁卸载（自动请求 UAC 提权）
#
# 删除：喂值服务 RAMFan、内核驱动服务 RAMFanPnP、System32\drivers\ramfan.sys。
# 同时默认关闭 testsigning（与 install.ps1 自动开启对应，需重启生效）。
# 注意：
#   - 内核驱动一旦加载，sc stop 可能返回 1052、ramfan.sys 被占用无法删除，
#     只能靠重启释放；服务项删除后重启不会自动再加载，无残留风险。
#   - 卸载不清除 NCT 最后写入的 Virtual_TEMP 值（保留到 NCT 复位/系统重启）。
#   - 不自动改内存完整性（HVCI）设置，不删测试证书（可能被其他测试驱动共用），
#     两者只打印命令，由你自行决定。Secure Boot 同理，需自行回 BIOS 开启。
#
# 用法（普通权限即可，自动请求 UAC）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
#   pwsh -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1   # PS7
#
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
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
    try {
        $p = Start-Process -FilePath $hostExe -ArgumentList $args2 -Verb RunAs -Wait -PassThru
    } catch {
        Write-Host "ERROR: 请求管理员权限失败或已被取消：$($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
    exit $p.ExitCode
}

# ---- 1. 停止并删除喂值服务 RAMFan ----
$q = sc.exe query RAMFan 2>&1
if ($q -match 'SERVICE_NAME: RAMFan') {
    '停止 RAMFan 服务 ...'
    sc.exe stop RAMFan 2>&1 | ForEach-Object { "  $_" }
    Start-Sleep -Seconds 3
    sc.exe delete RAMFan 2>&1 | ForEach-Object { "  $_" }
    Start-Sleep -Milliseconds 500
    $chk = sc.exe query RAMFan 2>&1
    if ($chk -match 'SERVICE_NAME: RAMFan') {
        Write-Host 'WARN: RAMFan 服务仍存在，请手动检查。' -ForegroundColor Yellow
    } else {
        Write-Host '服务 RAMFan 已删除。' -ForegroundColor Green
    }
    # 删除安装脚本复制到稳定路径的服务副本（日志保留）
    $svcCopy = 'C:\ProgramData\RAMFan\ramfan-service.exe'
    if (Test-Path $svcCopy) {
        Remove-Item $svcCopy -Force -ErrorAction SilentlyContinue
        Write-Host '已删除服务副本 C:\ProgramData\RAMFan\ramfan-service.exe'
    }
} else {
    Write-Host '服务 RAMFan 不存在，跳过。'
}

# ---- 2. 删除内核驱动服务 RAMFanPnP（尽力 stop；内核驱动可能停不掉 1052，仅尽力）----
$q = sc.exe query RAMFanPnP 2>&1
if ($q -match 'SERVICE_NAME: RAMFanPnP') {
    sc.exe stop RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }   # 1052 属预期
    '删除 RAMFanPnP 服务 ...'
    sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { Write-Host "  $_" }
    Start-Sleep -Milliseconds 500
    $chk = sc.exe query RAMFanPnP 2>&1
    if ($chk -match 'SERVICE_NAME: RAMFanPnP') {
        Write-Host '说明: RAMFanPnP 服务项已删除/标记删除；若驱动仍被加载，残留项在重启后移除。' -ForegroundColor Yellow
        Write-Host '      重启后仍存在才需手动处理。'
    } else {
        Write-Host '服务 RAMFanPnP 已删除。' -ForegroundColor Green
    }
} else {
    Write-Host '服务 RAMFanPnP 不存在，跳过。'
}

# ---- 3. 删除驱动文件（被占用时提示需重启释放）----
$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
if (Test-Path $dstSys) {
    try {
        Remove-Item $dstSys -Force -ErrorAction Stop
        Write-Host "已删除 $dstSys" -ForegroundColor Green
    }
    catch {
        Write-Host 'WARN: 无法删除 ramfan.sys（驱动二进制仍被加载）。' -ForegroundColor Yellow
        Write-Host '      服务已删除、重启不会自动再加载；重启后此文件即可移除，无残留风险。'
    }
}

# ---- 4. 关闭 testsigning（与安装脚本自动开启对应；Secure Boot 关闭后必须开启 testsigning 才能加载测试驱动，故一并还原）----
Write-Host ''
Write-Host '关闭 testsigning（bcdedit /deletevalue testsigning）...' -ForegroundColor Yellow
& "$env:WINDIR\System32\bcdedit.exe" /deletevalue testsigning 2>&1 | ForEach-Object { Write-Host "    $_" }
if ($LASTEXITCODE -ne 0) { Write-Host 'WARN: bcdedit /deletevalue testsigning 失败，请以管理员重试。' -ForegroundColor Yellow }

Write-Host ''
Write-Host '=== 卸载完成，需重启生效 ===' -ForegroundColor Green
Write-Host '重启后 testsigning 关闭、水印消失；两个服务项已删，驱动不会自动再加载。'
Write-Host 'Secure Boot：安装时若你在 BIOS 关闭过，卸载后请自行回 BIOS 重新开启（本脚本不碰 BIOS）。'
Write-Host '内存完整性（HVCI）与测试证书未自动改动；如需恢复/删除（自行决定）：'
Write-Host '  # 重新启用内存完整性：Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 → 开'
Write-Host '  或者（管理员）：'
Write-Host "    New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard' -Name EnableVirtualizationBasedSecurity -PropertyType DWord -Value 1 -Force"
Write-Host "    New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -Name Enabled -PropertyType DWord -Value 1 -Force"
Write-Host '  删除测试证书（其他测试驱动可能共用，谨慎）：'
Write-Host '  Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |'
Write-Host "    Where-Object { \$_.Subject -like '*RAMFanTestSign*' } | Remove-Item"
