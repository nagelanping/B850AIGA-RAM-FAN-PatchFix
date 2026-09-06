# RAMFan Virtual_TEMP 补丁（Windows 测试版安装说明）

修复 MAXSUN MS-iCraft B850 AIGA 主板「内存风扇自定义曲线重启后失效」问题。
驱动每 0.5s 读取 DIMM 温度，换算后持续写入 NCT6796D 的 `Virtual_TEMP`
（SIO 页 `0x0c` / reg `0x36`），使 `FAN5=MEM_FAN` 按 BIOS 曲线运行。
不修改 BIOS、风扇曲线或温度源。

## 适用主板（拒绝式校验，不匹配则驱动拒绝工作）

- MAXSUN MS-iCraft B850 AIGA（含 PCI `VEN_1022&DEV_790B` FCH SMBus）
- NCT chip id `0xd802`（NCT6796D-S / NCT6799D 兼容系列）

## 重要：这是测试签名版

本版本使用测试证书签名，**仅在关闭 Secure Boot + 开启 testsigning 的环境运行**。
不是正式发布签名（正式版需 Microsoft Attestation/WHQL 等可信签名）。

## 前置条件（由你自行判断并关闭，安装脚本只检测不自动改）

| 项 | 要求 | 如何关闭/开启 |
|---|---|---|
| Secure Boot | **OFF** | BIOS/UEFI 设置 → Secure Boot → Disabled |
| testsigning | **ON** | 管理员终端 `bcdedit /set testsigning on`，**重启** |
| 内存完整性（Memory Integrity / HVCI） | **OFF** | Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 → 关闭，重启 |
| 测试证书 `RAMFanTestSign` | 已导入本机受信任存储 | 驱动加载失败(577)时导入随包证书；受控实验仅限目标机 |

> 关闭 Secure Boot / 内存完整性会降低系统安全姿态，请知悉风险并自行决定。
> 卸载后可用 uninstall.ps1 提示的命令还原。
> 注意：执行 `bcdedit /set testsigning on` 后**必须重启**才生效——不重启时即使显示 Yes 驱动仍无法加载。

## 安装

1. 停止 HWiNFO、OpenHardwareMonitor、AIDA64 等会访问 SMBus/SIO 的监控工具
   （本补丁与它们共享端口，勿同时运行）。
2. 运行安装脚本（普通权限即可，脚本会自动请求 UAC 提升）：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```
   （装有 PowerShell 7 的用户同样可用：`pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1`）
   脚本会先检测 Secure Boot / testsigning / 内存完整性，任一不满足会明确报错并退出
   （不会替你修改设置）。检测通过后安装内核驱动 `RAMFanPnP` 与喂值服务 `RAMFan`
   （DEMAND_START，本次会话启动）。

## 验证

- 查看喂值日志：
  ```powershell
  Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20
  ```
  正常应看到类似 `FEED ok: max=35°C written=35 readback=35`，温度随 DIMM
  实际温度变化（跑内存压力测试可观察到升温）。
- 单次自检（驱动加载后）：
  ```powershell
  ramfan-service.exe --once
  ```
  成功返回 0，输出每槽状态与写回校验。

## 卸载

powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1   # 普通权限即可，自动请求 UAC

停止/删除服务与驱动文件。若 `ramfan.sys` 因驱动仍加载而无法删除，重启后
即可移除（服务项已删除，重启不会自动加载）。卸载不清除 NCT 最后写入值
（保留到 NCT 复位/系统重启），风扇会保持最后写入的转速直到重启。

## 已知边界与风险

- 内核驱动一旦加载，`sc stop` 可能返回 1052、无法热卸载，换新驱动需重启。
- 测试签名版只在目标机受控实验范围使用；正式发布需可信签名。
- 连续读取失败时保持最后写入值（不写 0°C、不写猜测值）；长时间失败使旧值
  过期是已知热安全风险，该版本不伪装成完整 fail-safe。
- 服务当前为 DEMAND_START（手动启动），未做开机自启。

## 组件

| 文件 | 说明 |
|---|---|
| `ramfan.sys` | 内核驱动（测试签名） |
| `ramfan-service.exe` | 喂值服务 / 自检工具 |
| `install.ps1` | 安装（检测 + 部署 + 自动提权） |
| `uninstall.ps1` | 卸载（自动提权） |
| `RAMFanTestSign.cer` | 测试证书（驱动加载失败 577 时导入；随包提供） |
| `INSTALL.md` | 本说明 |
