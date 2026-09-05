# test-enum-790b.ps1 — Enum\PCI 前缀枚举判定自检（无驱动加载、无端口访问）
# 镜像 driver/hw.c RamFanProbeFchSmbusController 的注册表前缀匹配逻辑：
# 遍历 HKLM\SYSTEM\CurrentControlSet\Enum\PCI 子键，若存在以
# VEN_1022&DEV_790B 开头的 hardware id 即判定 ControllerFound=1。
# 实机证据（2026-09-05）：Enum\PCI 下是完整 hardware id
# （VEN_1022&DEV_790B&SUBSYS_xxx&REV_xx），不存在裸 VEN_1022&DEV_790B 键。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Prefix = 'VEN_1022&DEV_790B'
$found = $false
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\PCI' -ErrorAction SilentlyContinue |
    ForEach-Object {
        if ($_.PSChildName -like "$Prefix*") {
            $found = $true
        }
    }

Write-Output "Enum\PCI contains $Prefix* : $found"

# 本机（目标机）必须存在；其它机器上该自检应正确返回与驱动一致的结论。
# 该脚本不修改任何设置、不访问端口。
if (-not $found) {
    Write-Output 'NOTE: 本机未枚举到 VEN_1022&DEV_790B（非目标机或控制器被禁用时属预期）。'
} else {
    Write-Output '790B controller enumerated; no hardware access performed.'
}
exit 0
