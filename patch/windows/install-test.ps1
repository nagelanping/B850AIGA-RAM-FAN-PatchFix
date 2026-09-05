# install-test.ps1 — 测试签名安装 ramfan 驱动（阶段 2 开发测试版）
# 仅用于开发测试，不绕过 Secure Boot 策略；Secure Boot 开启时拒绝安装并说明。
# 前置：build.ps1 构建成功；管理员 PowerShell。
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SignDriver,   # 用自签名测试证书给 ramfan.sys 签名（未签名时内核拒绝加载）
    [switch]$EnableTestSigning  # 允许执行 bcdedit /set testsigning on（需重启，显式选择）
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ---- 管理员检查 ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw '需要管理员权限。请右键 PowerShell -> 以管理员身份运行。'
}

# ---- Secure Boot 检查 ----
try {
    $secureBoot = Confirm-SecureBootUEFI
} catch {
    $secureBoot = $null  # 非 UEFI 或无法查询
}
if ($secureBoot -eq $true) {
    Write-Host "`n[!] 检测到 Secure Boot 已启用。" -ForegroundColor Yellow
    Write-Host "    测试签名驱动在 Secure Boot 开启时无法加载。" -ForegroundColor Yellow
    Write-Host "    选项：" -ForegroundColor Yellow
    Write-Host "      1) 在 BIOS 中临时关闭 Secure Boot（测试后恢复）" -ForegroundColor Yellow
    Write-Host "      2) 走正式签名路线（Microsoft Attestation/WHQL，阶段 4）" -ForegroundColor Yellow
    throw 'Secure Boot 已启用：测试签名安装中止。'
} elseif ($secureBoot -eq $false) {
    Write-Host "Secure Boot: 关闭（测试签名驱动可加载）"
} else {
    throw '无法查询 Secure Boot 状态：为安全起见中止测试签名安装。'
}

# ---- 测试签名开关检查 ----
$tsLine = bcdedit /enum '{current}' | Select-String 'testsigning' | Select-Object -First 1
$testsigning = if ($null -ne $tsLine) { $tsLine.ToString().Trim() } else { 'unknown' }
$stateDir = 'C:\ProgramData\RAMFan'
$statePath = Join-Path $stateDir 'test-state.json'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
@{ SecureBoot = [bool]$secureBoot; TestSigningWasOn = ($testsigning -match 'Yes') } | ConvertTo-Json | Set-Content -Path $statePath -Encoding UTF8
Write-Host "已保存测试前状态: $statePath"
Write-Host "测试签名策略: $testsigning"
if ($testsigning -notmatch 'Yes') {
    if ($EnableTestSigning) {
        Write-Host '执行: bcdedit /set testsigning on（重启后生效）' -ForegroundColor Yellow
        bcdedit /set testsigning on
        if ($LASTEXITCODE -ne 0) { throw 'bcdedit /set testsigning on 失败' }
        Write-Host '已启用测试签名。请重启后重新运行本脚本。' -ForegroundColor Yellow
        exit 0
    }
    throw '测试签名未开启：请加 -EnableTestSigning，重启后重新运行脚本。'
}

# ---- 定位构建产物 ----
$sys = Join-Path $root "driver\x64\$Configuration\ramfan.sys"
$exe = Join-Path $root "service\x64\$Configuration\ramfan-service.exe"
if (-not (Test-Path $sys)) { throw "未找到 $sys，请先运行 build.ps1" }
if (-not (Test-Path $exe)) { throw "未找到 $exe，请先运行 build.ps1" }

# ---- 可选：测试签名 ----
if ($SignDriver) {
    $winkits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $signtool = Get-ChildItem (Join-Path $winkits 'bin') -Recurse -Filter signtool.exe |
        Where-Object { $_.FullName -match 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw '未找到 signtool.exe（WDK 未安装？）' }

    $certName = 'RAMFanTestCert'
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -match $certName | Select-Object -First 1
    if (-not $cert) {
        Write-Host '创建自签名测试证书...'
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName" -CertStoreLocation Cert:\CurrentUser\My
    }
    # 内核 CI 校验查询 LocalMachine 信任存储（CurrentUser 不可见）：
    # 自签证书必须同时导入 TrustedPublisher 与 Root，否则 sc start 报签名无效 (577/STATUS_INVALID_IMAGE_HASH)
    $cerPath = Join-Path $env:TEMP "$certName.cer"
    Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Write-Host "测试证书已导入 LocalMachine 信任存储: $certName"
    & $signtool.FullName sign /v /s My /n $certName /t http://timestamp.digicert.com $sys
    if ($LASTEXITCODE -ne 0) {
        # 离线/时间戳失败时无时间戳重签
        Write-Host '带时间戳签名失败，尝试无时间戳签名...' -ForegroundColor Yellow
        & $signtool.FullName sign /v /s My /n $certName $sys
        if ($LASTEXITCODE -ne 0) { throw '签名失败' }
    }
    Write-Host "已签名: $sys"
} else {
    Write-Host "未签名。若内核拒绝加载，请加 -SignDriver 重新运行。" -ForegroundColor Yellow
}

# ---- 安装驱动服务（非 PnP KMDF：sc create，无需 INF） ----
Write-Host "`n=== 安装驱动服务 ==="
sc.exe stop RAMFanDriver 2>$null | Out-Null
sc.exe delete RAMFanDriver 2>$null | Out-Null

$driverDir = "$env:SystemRoot\System32\drivers"
Copy-Item $sys (Join-Path $driverDir 'ramfan.sys') -Force
sc.exe create RAMFanDriver type= kernel start= demand binPath= "$driverDir\ramfan.sys" DisplayName= "RAMFan VirtualTEMP driver (stage2)"
if ($LASTEXITCODE -ne 0) { throw 'sc create RAMFanDriver 失败' }
Write-Host '驱动服务已创建（DEMAND_START）。'

# ---- 安装用户态服务（阶段 2：FEED_ONCE 保留但受资源门禁，常驻喂值留阶段 3） ----
Write-Host "`n=== 安装用户态服务 ==="
$serviceDir = "$env:ProgramFiles\RAMFan"
New-Item -ItemType Directory -Path $serviceDir -Force | Out-Null
Copy-Item $exe (Join-Path $serviceDir 'ramfan-service.exe') -Force
& (Join-Path $serviceDir 'ramfan-service.exe') --install
if ($LASTEXITCODE -ne 0) { throw '服务注册失败' }

Write-Host "`n=== 验证步骤（阶段 2，资源模型解决前禁止执行） ==="
Write-Host "1. 当前不要启动驱动或服务进行真实硬件访问。"
Write-Host "2. 资源模型解决后再运行: & `"$serviceDir\ramfan-service.exe`" --once"
Write-Host "   当前预期 status=4，且不写 NCT。"
Write-Host "3. 卸载回滚:   pwsh -File uninstall.ps1"
Write-Host "`n注意：执行前停止其他会访问 0x295/0x296 的工具；常驻喂值留到阶段 3。"
