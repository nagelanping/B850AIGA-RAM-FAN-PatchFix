# ramfan（Windows 补丁）— 阶段 2：一次性写回闭环

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为“内存温度”时按 BIOS 曲线运行。

**当前阶段（阶段 2 草稿）**：接口和服务入口已保留，但写回被资源门禁明确阻断；
`FEED_ONCE` 在未获得 translated resources 前只返回硬件不可用，不访问端口、不写 NCT。
服务常驻仍保持阶段 1 的只读检查，0.5 秒常驻喂值留在阶段 3。
实机安装、驱动加载及 NCT/风扇验证按项目计划推迟到代码大致完成后。
## 组件

| 组件 | 文件 | 说明 |
| ---- | ---- | ---- |
| 内核驱动 | `driver/ramfan.c` | KMDF PnP upper-filter 骨架；绑定设计暂存于 `ramfan.inf.disabled`，只登记 translated port resources，不访问硬件 |
| 共享定义 | `driver/ramfan_ioctl.h` | IOCTL 与资源识别范围常量 |
| 用户态服务 | `service/ramfan-service.c` | `--once`、`--install`、`--uninstall` 当前拒绝；默认 SCM 模式仍只做阶段 2只读检查 |
| 构建 | `build.ps1` | 定位 VS/WDK，x64 Debug/Release；不复制 INF |
| 安装 | `install-test.ps1` | 当前主动拒绝安装，等待 INF/资源回调独立审查 |
| 卸载 | `uninstall.ps1` | 当前主动拒绝卸载，避免沿用旧非 PnP 清理逻辑 |
## 资源收集（机主执行）

`collect-resource-info.ps1` 只读查询 PnP 设备状态、资源属性和 `pnputil` 设备资源详情；不安装驱动、不访问 I/O port、不修改系统设置。资源模型实现前，先在目标机执行：

```powershell
Set-Location .\patch\windows
powershell.exe -NoProfile -File .\collect-resource-info.ps1 *> .\ramfan-resource-info.txt
Get-Content .\ramfan-resource-info.txt
```

请将 `ramfan-resource-info.txt` 内容回传。重点需要确认 AMD SMBus 设备、各 `PNP0C02` 实例的 `/resources` 输出，以及是否存在覆盖 `0x0b00-0x0b0f`、`0x0290-0x029f` 或 `0x002e-0x002f` 的资源。脚本不读取 `DEVPKEY_Device_TranslatedResourceList`；`pnputil /resources` 是当前系统可用的只读证据。
## 前置条件

- Windows 11 x64（10.0.26200 已验证），管理员 PowerShell。
- Visual Studio Build Tools 2022（C++ 负载）+ Windows SDK + WDK 10。
- 目标机硬件：AMD FCH SMBus 控制器 `VEN_1022&DEV_790B`；NCT chip id `0xd802`。
  （本机已记录：SMBus 基址 `0xb00-0xb0f` 在 ACPI `PNP0C02\700`；NCT 端口
  `0x295/0x296` 在 `PNP0C02\0` 声明的 `0x290-0x29F` 内。）

## 构建

```powershell
pwsh -File build.ps1 -Configuration Debug
```

产物：`driver\x64\Debug\ramfan.sys`、`service\x64\Debug\ramfan-service.exe`。

## 安装（当前暂停）

当前驱动已迁移为 PnP upper-filter 资源识别骨架，但 INF 绑定、PnP 回调生命周期和回滚流程尚未完成独立审查。为避免误改 `PNP0C02` 设备栈，`install-test.ps1` 当前会立即退出，不安装驱动、不修改 BCD、不注册服务。

服务程序的 `--install` / `--uninstall` 入口也已禁用；当前不会通过任何入口修改 SCM。

## PnP 绑定结论（当前暂停）

INF 的 Models section 只能按设备报告的 hardware ID/compatible ID 匹配；实机的 `ACPI\PNP0C02\700` 和 `ACPI\PNP0C02\0` 不能仅靠当前 `ACPI\PNP0C02` 通用 hardware ID 项安全地区分。因此不能把 `ramfan.inf.disabled` 改成“精确匹配两个实例”的安装包，也不能直接安装现有 upper-filter。

后续若继续使用 PNP0C02 upper-filter，必须同时满足：

1. INF 绑定范围和 filter 栈行为先在隔离环境验证；
2. 驱动运行时根据实际实例 ID 和 translated resources 做双重拒绝式筛选；
3. 非目标实例、资源缺失、资源歧义或资源角色不符时拒绝处理并保持不可用；在 filter 栈行为验证证明无副作用前不得安装；
4. 完成 catalog、签名、DriverStore 安装和可回滚卸载后，才能恢复安装入口。

控制设备队列显式运行在 `WdfExecutionLevelPassive`。已实现 active-user/rundown 生命周期门：事务开始在全局 wait-lock 下检查 Ready/冲突/Removing 并计数，`ReleaseHardware` 设置 Removing、阻止新事务、等待活动计数归零后才返回。真实端口事务接入时必须完整包在 begin/end 之间；当前仍不访问端口。
目标机当前只允许执行只读资源收集脚本；不要执行安装、启动驱动或 `--once`。

## 验证（当前禁止安装/加载）

资源识别骨架和 INF 尚未完成独立审查，当前不执行安装、驱动启动、服务启动或 `--once`。只允许运行 `collect-resource-info.ps1`。

当前版本无可执行验证命令；待 INF、资源共享状态和回滚流程完成后重新补充。

验收点：

- 当前资源门槛未满足时，`FEED_ONCE` 必须返回 status=4，且不访问 SMBus、不写 NCT。
- 资源模型解决并独立审查通过后，才验证“所有已映射 DIMM 读取成功后写入”；空槽 NACK 忽略，已装槽异常整轮失败。
- 资源模型解决并独立审查通过后，写入才允许严格限定为 page `0x0c` / reg `0x36`，写后读回一致并恢复原页；不写 page `0x09`。
- 对照历史 30°C/40°C 响应，确认 `pwm5/fan5` 单调变化；结束后重启确认 BIOS 曲线和温度源未改写。
- 测试期间停止 HWiNFO/OpenHardwareMonitor 等会访问 SMBus/NCT 端口的工具。

## 回滚（当前暂停）

当前 `uninstall.ps1` 会主动拒绝执行；它不会删除服务、驱动文件、注册表项或测试签名状态。INF 安装和对应的 filter/catalog 回滚流程完成后再启用。

驱动停止/卸载不清除 NCT 最后一次写入值；不修改 BIOS、曲线或温度源。

## 已知风险与边界（阶段 2）

- **外部并发**：标准 SIO `0x2e/0x2f` 与自定义口 `0x295/0x296` 是主板级共享端口。
  实机测试前停止 HWiNFO/OpenHardwareMonitor 等会访问这些端口的工具；驱动内部串行队列
- **资源模型当前实现中**：驱动已改为 PnP upper-filter 骨架，只登记 `EvtDevicePrepareHardware` 收到的 translated port resources；旧 PCI 扫描、固定 `0xb00` 回退和硬件访问路径已断开。当前 `FEED_ONCE` 仍在任何硬件访问前返回 `RAMFAN_FEED_HW_UNAVAILABLE`。后续需完成资源共享状态、INF 安装和独立审查。
- **SMBus `0x04` 语义未充分区分**：驱动不再把它统一当作空槽 NACK；遇到该状态时
  映射失败或已映射槽整轮失败，不得用剩余 DIMM 降速。
- **失败保持旧值**：读取或写回失败时不写 0°C、不写猜测值；连续失败导致旧值过期是已知热安全风险。
- **测试签名版不伪装正式发布**：正式交付需 Microsoft Attestation/WHQL 等可信签名。

阶段 3 才启用 0.5 秒常驻喂值和自动启动；实机验证通过前不切换。
