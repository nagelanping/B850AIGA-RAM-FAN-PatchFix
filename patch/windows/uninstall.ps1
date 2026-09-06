# uninstall.ps1 — RAMFan Virtual_TEMP 补丁卸载（自动请求 UAC 提权）
#
# 删除：喂值服务 RAMFan、内核驱动服务 RAMFanPnP、System32\drivers\ramfan.sys。
# 注意：
#   - 内核驱动一旦加载，sc stop 可能返回 1052、ramfan.sys 被占用无法删除，
#     只能靠重启释放；服务项删除后重启不会自动再加载，无残留风险。
#   - 卸载不清除 NCT 最后写入的 Virtual_TEMP 值（保留到 NCT 复位/系统重启）。
#   - 还原安全设置：默认只打印命令，不自动改。加 -RestoreSecurity 则自动还原
#     testsigning 与内存完整性（脚本安装时自动开启/关闭的项），需重启生效；
#     测试证书删除有风险（其他测试驱动可能共用），仅打印命令由你决定。
#
# 用法（普通权限即可，自动请求 UAC）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1 -RestoreSecurity
#   pwsh -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1   # PS7
#
[CmdletBinding()]
param(
    # 还原脚本安装时自动改的安全设置（testsigning/内存完整性），需重启生效
    [switch]$RestoreSecurity
)
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
    if ($RestoreSecurity) { $args2 += '-RestoreSecurity' }
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

if ($RestoreSecurity) {
    Write-Host ''
    Write-Host '还原脚本自动修改的安全设置 ...' -ForegroundColor Yellow
    Write-Host '  关闭 testsigning（bcdedit /deletevalue testsigning）...'
    & "$env:WINDIR\System32\bcdedit.exe" /deletevalue testsigning 2>&1 | ForEach-Object { Write-Host "    $_" }
    $dgKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
    New-Item -Path $dgKey -Force | Out-Null
    New-ItemProperty -Path $dgKey -Name 'EnableVirtualizationBasedSecurity' -PropertyType DWord -Value 1 -Force | Out-Null
    $scKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
    New-Item -Path $scKey -Force | Out-Null
    New-ItemProperty -Path $scKey -Name 'Enabled' -PropertyType DWord -Value 1 -Force | Out-Null
    Write-Host '  已配置内存完整性重新启用（等同 Windows 安全中心界面开启）。'
    Write-Host ''
    Write-Host '=== 安全设置已还原，需重启生效 ===' -ForegroundColor Green
    Write-Host 'Secure Boot：安装要求你手动关闭过，请自行回 BIOS 重新开启（本脚本不碰 BIOS）。'
    Write-Host '测试证书 RAMFanTestSign（可选删除，其他测试驱动可能共用）：'
    Write-Host '  Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |'
    Write-Host "    Where-Object { \$_.Subject -like '*RAMFanTestSign*' } | Remove-Item"
} else {
    Write-Host ''
    Write-Host '=== 卸载完成 ==='
    Write-Host '如需还原安装时自动修改的安全设置，加 -RestoreSecurity 重跑本脚本（需重启生效）：'
    Write-Host '  powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1 -RestoreSecurity'
    Write-Host 'Secure Boot：若你手动关闭过，请自行回 BIOS 重新开启。'
}
