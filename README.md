# MS-iCraft B850 AIGA 内存风扇曲线修复

修复 MAXSUN MS-iCraft B850 AIGA / 铭瑄 B850 瑷珈 主板固件 Bug：系统重启后，`FAN5=MEM_FAN` 内存风扇转速曲线失效 / 不再跟随内存温度。

Linux 版本为已实机验证的正式版。Windows 版本为**测试签名**版，提供一键安装脚本与开机自启。发布包见本仓库 Releases 页面。

## 问题根因

BIOS 将内存风扇温度源配置为 NCT6796D 的 `Virtual_TEMP`。该通道没有硬件数据，固件只在 BIOS 打开内存风扇曲线页面时写入温度，重启后停止写入，风扇回到低转速。

本补丁持续读取 DIMM 温度并写入 NCT `Virtual_TEMP`，让内存风扇按 BIOS 中原有曲线运行。不修改风扇曲线、温度源、BIOS 或固件。

## 下载与安装

从本仓库 GitHub Releases 页面下载对应平台的发布包：

| 平台    | 发布包                                            | 说明                                                       |
| ------- | ------------------------------------------------- | ---------------------------------------------------------- |
| Linux   | `B850AIGA-RAM-FAN-PatchFix_Linux.tar.gz`        | 正式版，需`spd5118` hwmon                                |
| Windows | `B850AIGA-RAM-FAN-PatchFix_Windows_TestSign.7z` | 测试签名，非正式签名；随包脚本一键配置环境并安装 |

解压后，按包内 `INSTALL.md` 安装。

### Windows 说明

该版本使用自签测试证书。安装脚本 `install.ps1` 会**自动开启 testsigning、关闭内存完整性、导入测试证书**；需要重启时脚本会提示，重启后再次运行即完成安装。**Secure Boot 需在 BIOS/UEFI 手动关闭**（脚本无法改），脚本检测到开启会提示。

关闭 Secure Boot 与内存完整性会降低系统安全姿态，请知悉风险后自行决定是否安装。卸载可用 `uninstall.ps1`；需要还原安全设置时加 `-RestoreSecurity`。

**测试模式水印**：testsigning 开启期间，桌面右下角会显示“测试模式”水印，这是 Windows 对测试签名系统的标识，属预期现象。水印随 testsigning 关闭而消失（关闭 testsigning 后本补丁驱动无法再加载，等价于卸载还原）。本补丁不提供隐藏水印的工具——隐藏需修改系统 UI 组件，不属补丁职责，也不推荐使用第三方水印隐藏工具。

## 验证

- Linux：安装见包内 `INSTALL.md`；停止服务用 systemd。
- Windows：安装脚本完成即启动喂值服务 `RAMFan`；单次自检用 `ramfan-service.exe --once`，成功返回 0。

已知边界：服务停止不清除最后一次写入的 `Virtual_TEMP` 值，该值保持到 NCT 复位或系统重启。连续读取失败时保持旧值；长时间失败使旧值过期，该行为不是完整 fail-safe。

## 已验证范围

- 主板：MAXSUN MS-iCraft B850 AIGA（Linux 实机）；Windows 目标机同型号，含 PCI `VEN_1022&DEV_790B` FCH SMBus、NCT chip id `0xd802`。
- 温度源：Linux 内核 `spd5118` hwmon；Windows FCH SMBus SPD word-read。
- NCT 芯片：实测兼容 NCT6796D-S / NCT6799D 系列。

其他主板、其他 NCT 型号或缺少上述温度源的系统不在已验证范围。

## 许可证

MIT License。分发需保留版权。
