# uninstall.ps1 — RAMFan Virtual_TEMP 补丁卸载（自动请求 UAC 提权）
#
# 删除：喂值服务 RAMFan、内核驱动服务 RAMFanPnP、System32\drivers\ramfan.sys。
# 注意：
#   - 内核驱动一旦加载，sc stop 可能返回 1052、ramfan.sys 被占用无法删除，
#     只能靠重启释放；服务项删除后重启不会自动再加载，无残留风险。
#   - 卸载不清除 NCT 最后写入的 Virtual_TEMP 值（保留到 NCT 复位/系统重启）。
#   - 不还原 Secure Boot / testsigning / 内存完整性设置（由用户自行决定还原）。
#
# 用法（普通权限即可，自动请求 UAC）：pwsh -ExecutionPolicy Bypass -File .\uninstall.ps1

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

Write-Host ''
Write-Host '=== 卸载完成 ==='
Write-Host '如需彻底还原开发环境（可选，自行决定）：'
Write-Host '  bcdedit /deletevalue testsigning   # 关闭 testsigning（需重启）'
Write-Host '  BIOS/UEFI 中重新开启 Secure Boot'
Write-Host '  Windows 安全中心重新开启内存完整性'
