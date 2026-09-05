# test-identity-model.ps1 — 编译并运行身份门禁判定纯逻辑自检（无驱动加载、无端口）
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw '未找到 vswhere，无法定位 MSVC。'
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw '未找到带 C++ 工具集的 Visual Studio 安装。'
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "未找到 x64 MSVC 环境脚本: $vcvars"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ('ramfan-identity-model-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $exe = Join-Path $temp 'identity-model-test.exe'
    $test = Join-Path $root 'test-identity-model.c'
    $model = Join-Path $root 'driver\identity_model.c'
    $command = '"{0}" x64 && cd /d "{1}" && cl.exe /nologo /W4 /WX /utf-8 /std:c11 /I"{2}" "{3}" "{4}" /Fe:"{5}"' -f `
        $vcvars, $temp, $root, $test, $model, $exe

    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "身份门禁自检编译失败（exit $LASTEXITCODE）。"
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw "身份门禁自检失败（exit $LASTEXITCODE）。"
    }
}
finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
