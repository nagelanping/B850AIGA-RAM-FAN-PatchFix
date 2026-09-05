# identity-gate-rollback.ps1 — 身份门禁阶段回滚（管理员，机主执行）
# 停止并删除 RAMFanPnP 内核服务、删除 System32\drivers\ramfan.sys。
# 不改 testsigning / 证书（如需彻底清理用 experiment-b-rollback.ps1 -RemoveCert）。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'

$q = sc.exe query RAMFanPnP 2>&1
if ($q -match 'RUNNING|STOP_PENDING') {
    '停止 RAMFanPnP ...'
    sc.exe stop RAMFanPnP 2>&1 | ForEach-Object { "  $_" }
    Start-Sleep -Milliseconds 500
}
'删除 RAMFanPnP 服务 ...'
sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { "  $_" }

$dstSys = Join-Path $env:WINDIR 'System32\drivers\ramfan.sys'
if (Test-Path $dstSys) {
    Remove-Item $dstSys -Force
    "已删除 $dstSys"
}

# 控制设备名是 \Device\RamFanVirtTemp（非文件系统对象，驱动卸载时自动删除），
# 无需注册表清理。确认驱动对象不再存在：
$still = sc.exe query RAMFanPnP 2>&1
if ($still -match 'SERVICE_NAME: RAMFanPnP') {
    Write-Host 'WARN: RAMFanPnP 服务仍存在，请手动检查。'
} else {
    Write-Host '回滚完成：RAMFanPnP 已删除，ramfan.sys 已移除。'
}
Write-Host '身份门禁阶段的控制设备在驱动卸载时由 EvtDriverUnload 删除，无残留设备对象。'
