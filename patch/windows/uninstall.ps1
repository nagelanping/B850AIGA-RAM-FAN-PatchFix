# uninstall.ps1 — 卸载 ramfan 驱动与服务（回滚）
# 管理员 PowerShell 运行。恢复原状，不触碰 BIOS/曲线配置。
$ErrorActionPreference = 'Stop'

Write-Host '=== 停止并删除用户态服务 ==='
$svcExe = "$env:ProgramFiles\RAMFan\ramfan-service.exe"
if (Test-Path $svcExe) {
    sc.exe stop RAMFan 2>$null | Out-Null
    & $svcExe --uninstall
    Remove-Item "$env:ProgramFiles\RAMFan" -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '=== 停止并删除驱动服务 ==='
sc.exe stop RAMFanDriver 2>$null | Out-Null
sc.exe delete RAMFanDriver 2>$null | Out-Null


$sys = "$env:SystemRoot\System32\drivers\ramfan.sys"
if (Test-Path $sys) {
    Remove-Item $sys -Force
}


$statePath = 'C:\ProgramData\RAMFan\test-state.json'
if (Test-Path $statePath) {
    $state = Get-Content $statePath -Raw | ConvertFrom-Json
    if (-not [bool]$state.TestSigningWasOn) {
        Write-Host '恢复测试签名状态: bcdedit /set testsigning off'
        bcdedit /set testsigning off | Out-Null
        if ($LASTEXITCODE -ne 0) { throw '恢复 testsigning 状态失败' }
    }
    Remove-Item $statePath -Force
}
Write-Host '=== 可选清理 ==='
Write-Host '若不再需要测试签名，可执行: bcdedit /set testsigning off（需重启）'
Write-Host '卸载完成。驱动停止不会清除 NCT 最后一次写入值（当前资源门禁下未写 NCT）。'
