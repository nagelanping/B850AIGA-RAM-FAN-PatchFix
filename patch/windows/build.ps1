# build.ps1 — 构建 ramfan 驱动与服务（x64 Debug/Release）
# 前置：已安装 VS Build Tools 2022（含 C++ 负载）与 WDK 10。
# 用法：pwsh -File build.ps1 [-Configuration Debug|Release]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ---- 定位 VS / MSBuild ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw '未找到 vswhere（VS Build Tools 未安装？）'
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw '未找到带 C++ 工具集的 Visual Studio 安装'
}
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "未找到 MSBuild.exe: $msbuild"
}

# ---- 检查 WDK ----
$winkits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
if (-not (Test-Path $winkits)) {
    throw '未找到 Windows Kits\10（WDK 未安装？）'
}

Write-Host "VS:     $vsPath"
Write-Host "MSBuild: $msbuild"
Write-Host "WDK:    $winkits"
Write-Host "配置:   $Configuration x64"

# ---- 构建 ----
function Invoke-MsBuild([string]$Project) {
    Write-Host "`n=== 构建 $Project ==="
    # WDK 26100 只带 Microsoft.DriverKit.Build.Tasks.17.0.dll，
    # VS 2026 (MSBuild 18) 默认按 VisualStudioVersion=18.0 找任务程序集；固定 17.0 加载兼容版本。
    & $msbuild $Project /p:Configuration=$Configuration /p:Platform=x64 /p:VisualStudioVersion=17.0 /m /v:m
    if ($LASTEXITCODE -ne 0) {
        throw "构建失败: $Project (exit $LASTEXITCODE)"
    }
}

Invoke-MsBuild (Join-Path $root 'driver\ramfan.vcxproj')
Invoke-MsBuild (Join-Path $root 'service\ramfan-service.vcxproj')

Write-Host "`n构建完成。产物："
Write-Host "  driver\x64\$Configuration\ramfan.sys（未签名）"
Write-Host "  service\x64\$Configuration\ramfan-service.exe"
Write-Host "身份门禁阶段：安装/加载需由机主在目标机执行（测试签名 + sc 加载），本脚本不安装。"
