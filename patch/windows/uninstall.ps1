# uninstall.ps1 — 当前暂停
# 当前工作区只有未签名、不可安装的 PnP 资源识别骨架。
# 禁止沿用旧的非 PnP 清理逻辑，以免误删其他测试服务或驱动文件。
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
throw '当前没有可执行的 PnP 回滚流程；未执行任何系统修改。'
