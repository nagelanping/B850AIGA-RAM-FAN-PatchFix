# experiment-b-prep.ps1 — 历史工具（实验 B：PnP 绑定测试，已证伪归档）
# 仍可复用：测试证书 RAMFanTestSign 创建/信任、testsigning 开启、Inf2Cat/catalog 与 /ph 签名。
# 当前身份门禁阶段请使用 identity-gate-prep.ps1 / identity-gate-rollback.ps1。
# 前置：已在 patch/windows 下构建 Release；管理员；Secure Boot False。
[CmdletBinding()]
param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$log = Join-Path $root 'experiment-b-logs'
New-Item -ItemType Directory -Path $log -Force | Out-Null
$logFile = Join-Path $log 'prep.log'
Start-Transcript -Path $logFile -Force

function Fail([string]$msg) {
    Stop-Transcript | Out-Null
    throw $msg
}

# ---- 1. 环境检查 ----
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail '需要管理员权限。'
}
if (Confirm-SecureBootUEFI) {
    Fail 'Secure Boot 为 True：测试签名驱动不能加载。如需继续必须先关闭 Secure Boot（本机当前为 False）。'
}
Get-Process | Where-Object { $_.ProcessName -match 'hwinfo|openhardwaremonitor|cpuz|aida64|librehardware' } |
    ForEach-Object { Fail "检测到会访问端口的进程 $($_.ProcessName)，请先停止。" }

$sys = Join-Path $root "driver\x64\$Configuration\ramfan.sys"
if (-not (Test-Path $sys)) { Fail "未找到驱动: $sys（先运行 build.ps1 -Configuration $Configuration）" }

$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$signtool = Get-ChildItem (Join-Path $kits 'bin') -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
$inf2cat = Join-Path $kits 'bin\10.0.26100.0\x86\Inf2Cat.exe'
if (-not $signtool) { Fail '未找到 signtool。' }
if (-not (Test-Path $inf2cat)) { Fail '未找到 Inf2Cat。' }
"signtool=$($signtool.FullName)"
"inf2cat=$inf2cat"

# ---- 2. 测试证书（Machine store My/Root/TrustedPublisher）----
$certName = 'RAMFanTestSign'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like "*CN=$certName*" } | Select-Object -First 1
if (-not $cert) {
    "创建新测试证书 $certName ..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName,O=B850AIGA-Dev" `
        -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(2)
}
"cert thumbprint=$($cert.Thumbprint)"
foreach ($store in 'Root','TrustedPublisher') {
    $exists = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if (-not $exists) {
        $tmpCer = Join-Path $env:TEMP "ramfan-$store.cer"
        Export-Certificate -Cert $cert -FilePath $tmpCer | Out-Null
        Import-Certificate -FilePath $tmpCer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Remove-Item $tmpCer -Force
        "imported to $store"
    } else {
        "$store already trusted"
    }
}

# ---- 3. 暂存目录：INF + sys + catalog ----
$staging = Join-Path $log 'package'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging | Out-Null

$infTemplate = Join-Path $root 'driver\ramfan.inf.disabled'
$infContent = Get-Content $infTemplate -Raw
# 启用为可安装 INF：修正头部注释不含误导？保留；只需注入 CatalogFile 与当前日期版本
if ($infContent -notmatch 'CatalogFile=') {
    $infContent = $infContent -replace '(\[Version\][\r\n]+Signature=.*\r?\n)', "`$1CatalogFile=ramfan.cat`r`n"
}
$infContent = $infContent -replace 'DriverVer=.*', "DriverVer=09/05/2026,0.3.0.0"
$infPath = Join-Path $staging 'ramfan.inf'
Set-Content -Path $infPath -Value $infContent -Encoding Ascii
Copy-Item $sys (Join-Path $staging 'ramfan.sys')

# ---- 4. catalog + 签名 ----
Push-Location $staging
try {
    "运行 Inf2Cat ..."
    & $inf2cat /driver:$staging /os:10_X64 /verbose 2>&1 | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { Fail "Inf2Cat 失败 (exit $LASTEXITCODE)" }
    if (-not (Test-Path (Join-Path $staging 'ramfan.cat'))) { Fail '未生成 ramfan.cat' }
} finally {
    Pop-Location
}

# .cat 无页哈希；.sys 内核加载需要页哈希（实测缺 /ph 报错 577）
foreach ($target in @((Join-Path $staging 'ramfan.cat'), (Join-Path $staging 'ramfan.sys'))) {
    $flags = '/fd sha256'
    if ($target -like '*.sys') { $flags = '/fd sha256 /ph' }
    & $signtool.FullName sign /v /sm /s My /n $certName $flags $target 2>&1 | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { Fail "签名失败: $target (exit $LASTEXITCODE)" }
}

# 校验签名
& $signtool.FullName verify /kp (Join-Path $staging 'ramfan.cat') 2>&1 | Select-String -Pattern 'Successfully verified|verified|error' | ForEach-Object { "verify: $_" }

# ---- 5. 启用测试签名并记录原状态 ----
$bcd = & "$env:WINDIR\System32\bcdedit.exe" /enum '{current}' 2>&1
$wasOn = ($bcd | Select-String 'testsigning\s+Yes' -Quiet)
if (-not $wasOn) {
    "启用 bcdedit testsigning ..."
    bcdedit /set testsigning on 2>&1 | ForEach-Object { "  $_" }
    "testsigning=on" | Set-Content (Join-Path $log 'prep-bcd.txt')
} else {
    'testsigning 已开启'
}

Stop-Transcript | Out-Null
Write-Host ""
Write-Host "=== 实验 B 准备完成 ==="
Write-Host "包目录 : $staging"
Write-Host "日志    : $logFile"
Write-Host ""
Write-Host "下一步：重启系统使 testsigning 生效，然后以管理员运行："
Write-Host "  pwsh -File .\patch\windows\experiment-b-verify.ps1"
