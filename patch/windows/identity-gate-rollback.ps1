# identity-gate-rollback.ps1 — 身份门禁阶段回滚（管理员，机主执行）
# 删除 RAMFanPnP 服务、删除 System32\drivers\ramfan.sys。
# 注意：内核驱动二进制一旦加载，sc stop 可能返回 1052、文件被占用，
# 只能通过重启释放；服务注册表项删除后，重启不会再次自动加载该驱动。
# 不改 testsigning / 证书（如需彻底清理用 experiment-b-rollback.ps1 -RemoveCert）。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# ---- 1. 删除服务（先尝试停止，失败也继续删除服务项）----
$q = sc.exe query RAMFanPnP 2>&1
if ($q -match 'SERVICE_NAME: RAMFanPnP') {
    $stop = sc.exe stop RAMFanPnP 2>&1
    if ($stop -notmatch 'STOP_PENDING|STOPPED') {
        "注意：sc stop 未进入停止流程（1052 等）；内核二进制仍可能加载，将靠重启释放。"
    }
    '删除 RAMFanPnP 服务 ...'
    sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { "  $_" }
    $svcCheck = sc.exe query RAMFanPnP 2>&1
    if ($svcCheck -match 'SERVICE_NAME: RAMFanPnP') {
        Write-Host 'WARN: RAMFanPnP 服务仍存在，请手动检查。'
    } else {
        Write-Host '服务 RAMFanPnP 已删除。'
    }
} else {
    Write-Host '服务 RAMFanPnP 不存在，跳过。'
}

# ---- 2. 删除驱动文件（被占用时明确提示需重启释放）----
$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
if (Test-Path $dstSys) {
    try {
        Remove-Item $dstSys -Force -ErrorAction Stop
        Write-Host "已删除 $dstSys"
    }
    catch {
        Write-Host "WARN: 无法删除 $dstSys（驱动二进制仍被加载）。"
        Write-Host '      服务已删除、驱动不会再次自动加载；重启后此文件即可移除，无残留风险。'
    }
}

Write-Host '回滚完成（控制设备在驱动卸载时由 EvtDriverUnload 删除，无残留设备对象）。'
