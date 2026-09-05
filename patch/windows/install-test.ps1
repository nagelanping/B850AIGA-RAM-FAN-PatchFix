# install-test.ps1 — 暂停安装（PnP 资源识别骨架）
# 当前版本仅完成编译期 PnP 回调骨架；INF 绑定和实机加载验证尚未通过独立审查。
# 为避免误将骨架安装到 PNP0C02 设备，本脚本现在拒绝执行任何安装操作。
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
throw '当前版本仅支持资源识别骨架，安装/加载流程尚未完成；未执行任何系统修改。'
