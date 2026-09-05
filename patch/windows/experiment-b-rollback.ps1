# experiment-b-rollback.ps1 — 实验 B 回滚（管理员）
# 卸载驱动包、清除 PNP0C02 UpperFilters 残留、删除服务、恢复 testsigning。
[CmdletBinding()]
param(
    [switch]$RemoveCert,        # 同时删除 RAMFanTestSign 证书（My/Root/TrustedPublisher）
    [switch]$KeepTestSigning    # 保留 bcdedit testsigning=on
)

$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$log = Join-Path $root 'experiment-b-logs'
New-Item -ItemType Directory -Path $log -Force | Out-Null
$logFile = Join-Path $log 'rollback.log'
Start-Transcript -Path $logFile -Force

# ---- 1. 删除驱动包（含设备卸载）----
"=== pnputil /delete-driver ==="
$drivers = pnputil /enum-drivers 2>&1
$oemName = $null
for ($i = 0; $i -lt $drivers.Count; $i++) {
    if ($drivers[$i] -match '原始名称|Original Name|Published Name') { continue }
    if ($drivers[$i] -match 'ramfan\.inf') {
        # 向上找最近一个 oem 行
        for ($j = $i; $j -ge 0; $j--) {
            if ($drivers[$j] -match 'oem(\d+)\.inf') { $oemName = $drivers[$j].Trim(); break }
        }
    }
}
if ($oemName) {
    "删除 $oemName ..."
    pnputil /delete-driver $oemName /uninstall 2>&1 | ForEach-Object { "  $_" }
    $found = $true
} else {
    '未在 DriverStore 找到 ramfan.inf 驱动包。'
}

# ---- 2. 清除 PNP0C02 UpperFilters 残留并重启节点 ----
$paths = Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\PNP0C02' -ErrorAction SilentlyContinue
foreach ($node in $paths) {
    $key = $node.PSPath
    $uf = (Get-ItemProperty $key -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if ($uf) {
        "节点 $($node.PSChildName) UpperFilters=$uf"
        # 只删除引用 RAMFanPnP 的值；若只有它则删除整个值
        if ($uf -eq 'RAMFanPnP') {
            Remove-ItemProperty $key -Name UpperFilters -ErrorAction SilentlyContinue
            "  已移除 UpperFilters"
        } else {
            $rest = ($uf -split ',') | Where-Object { $_ -ne 'RAMFanPnP' }
            Set-ItemProperty $key -Name UpperFilters -Value ($rest -join ',')
            "  已从 UpperFilters 移除 RAMFanPnP，剩余 $($rest -join ',')"
        }
    }
}

# ---- 3. 删除服务 ----
"=== sc delete RAMFanPnP ==="
sc.exe delete RAMFanPnP 2>&1 | ForEach-Object { "  $_" }

# ---- 4. 重启相关节点恢复 machine.inf 栈 ----
Start-Sleep -Seconds 2
foreach ($inst in 'ACPI\PNP0C02\700', 'ACPI\PNP0C02\0') {
    pnputil /restart-device $inst 2>&1 | ForEach-Object { "  restart $inst : $_" }
}

# ---- 5. testsigning 恢复 ----
if (-not $KeepTestSigning) {
    $bcd = & "$env:WINDIR\System32\bcdedit.exe" /enum '{current}' 2>&1
    if ($bcd | Select-String 'testsigning\s+Yes' -Quiet) {
        "关闭 testsigning ..."
        bcdedit /set testsigning off 2>&1 | ForEach-Object { "  $_" }
        '注意：testsigning 已关闭，重启后生效。'
    } else {
        'testsigning 已处于关闭状态。'
    }
}

# ---- 6. 可选删除证书 ----
if ($RemoveCert) {
    Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like '*CN=RAMFanTestSign*' } |
        Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Subject -like '*CN=RAMFanTestSign*' } |
        Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Subject -like '*CN=RAMFanTestSign*' } |
        Remove-Item -ErrorAction SilentlyContinue
    '已删除 RAMFanTestSign 证书。'
}

Stop-Transcript | Out-Null
Write-Host ""
Write-Host '回滚完成，日志：' $logFile
Write-Host '若 testsigning 被关闭，请重启系统生效。'
