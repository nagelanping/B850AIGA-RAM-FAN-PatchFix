# RAM-FAN Virtual_TEMP 补丁（Windows 安装说明）

修复 MAXSUN MS-iCraft B850 AIGA 主板「内存风扇自定义曲线重启后失效」问题。
驱动每 0.5s 读取 DIMM 温度，换算后持续写入 NCT6796D 的 `Virtual_TEMP`
（SIO 页 `0x0c` / reg `0x36`），使 `FAN5=MEM_FAN` 按 BIOS 曲线运行。
不修改 BIOS、风扇曲线或温度源。

## 适用主板（拒绝式校验，不匹配则驱动拒绝工作）

- MAXSUN MS-iCraft B850 AIGA（含 PCI `VEN_1022&DEV_790B` FCH SMBus）
- NCT chip id `0xd802`（NCT6796D-S / NCT6799D 兼容系列）

## 重要：这是测试签名版

本版本使用测试证书签名，**仅在关闭 Secure Boot + 开启 testsigning 的环境运行**。
不是官方签名驱动（无官方认证计划），仅适合能接受相应安全设置变更的用户。

## 前置条件（脚本一键自动配置，Secure Boot 除外）

运行安装脚本会自动：**开启 testsigning、关闭内存完整性、导入测试证书**，并提示重启后完成安装。**Secure Boot 无法用脚本关闭，须在 BIOS/UEFI 手动关闭**。以下为各安全设置说明：

| 项                         | 安装脚本要求  | 说明                                                            |
| -------------------------- | ------------- | --------------------------------------------------------------- |
| Secure Boot                | **OFF** | 需在 BIOS/UEFI 手动关闭（各主板菜单不同）；脚本检测到 ON 会提示 |
| testsigning                | **ON**  | 脚本自动`bcdedit /set testsigning on`，重启生效               |
| 内存完整性（HVCI）         | **OFF** | 脚本自动关闭（注册表），重启生效                                |
| 测试证书`RAMFanTestSign` | 已导入        | 脚本自动导入包内`RAMFanTestSign.cer` 到 Root/TrustedPublisher |

> 关闭 Secure Boot / 内存完整性会降低系统安全姿态。这些是本补丁测试签名驱动的运行前提，由用户决定是否安装。
> 卸载后可用 `uninstall.ps1 -RestoreSecurity` 自动还原 testsigning 与内存完整性；Secure Boot 需自行回 BIOS 开启。

## 安装

1. 在 BIOS/UEFI 中关闭 Secure Boot（脚本无法自动改，其余安全设置由脚本处理）。
2. 停止 HWiNFO、OpenHardwareMonitor、AIDA64 等会访问 SMBus/SIO 的监控工具
   （本补丁与它们共享端口，勿同时运行）。
3. 运行安装脚本（普通权限即可，脚本会自动请求 UAC 提升）：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```

   脚本自动处理：testsigning 开启、内存完整性关闭、测试证书导入。若需要重启，
   脚本会提示；**重启后再次运行同一命令**完成驱动 `RAMFanPnP` 与喂值服务 `RAMFan`
   的安装（两者均为 AUTO_START，开机自动恢复喂值）。

## 验证

- 查看喂值日志：

  ```powershell
  Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20
  ```

  正常应看到类似 `FEED ok: max=35°C written=35 readback=35`，温度随 DIMM
  实际温度变化（跑内存压力测试可观察到升温）。
- 单次自检（驱动加载后）：

  ```powershell
  .\ramfan-service.exe --once   # 在解压目录（或 C:\ProgramData\RAMFan\）执行
  ```

  成功返回 0，输出各槽状态与写回校验。

## 卸载

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

停止/删除服务与驱动文件。若 `ramfan.sys` 因驱动仍加载而无法删除，重启后
即可移除（服务项已删除，重启不会自动加载）。卸载不清除 NCT 最后写入值
（保留到 NCT 复位/系统重启），风扇会保持最后写入的转速直到重启。

还原安装时自动修改的安全设置（可选）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1 -RestoreSecurity   # 需重启生效
```

`-RestoreSecurity` 自动还原 testsigning（关闭）与内存完整性（重新启用）。Secure Boot 需自行回 BIOS 开启。测试证书删除有风险（其他测试驱动可能共用），脚本只打印删除命令。

## 已知边界与风险

- 内核驱动一旦加载，`sc stop` 可能返回 1052、无法热卸载，换新驱动需重启。
- 测试签名版非正式发布签名；需关闭 Secure Boot/testsigning/内存完整性运行，安全姿态低于默认。
- 连续读取失败时保持最后写入值；可能的长时间失败会使旧值过期。
- 两个服务均为 AUTO_START（开机自启）：卸载脚本删除服务项后，重启不再自动加载。

## 组件

| 文件 | 说明 |
| ---------------------- | --------------------------------------------- |
| `ramfan.sys` | 内核驱动（测试签名） |
| `ramfan-service.exe` | 喂值服务 / 自检工具 |
| `install.ps1` | 安装（检测 + 部署 + 自动提权） |
| `uninstall.ps1` | 卸载（自动提权） |
| `RAMFanTestSign.cer` | 测试证书（驱动加载失败 577 时导入；随包提供） |
| `INSTALL.md` | 本说明 |
