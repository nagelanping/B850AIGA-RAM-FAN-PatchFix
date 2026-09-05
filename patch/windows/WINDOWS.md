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
| 内核驱动 | `driver/ramfan.c` `driver/hw.c` | 非 PnP KMDF；`\Device\RamFanVirtTemp`；阶段 1 只读探测；阶段 2 写回暂被资源门禁阻断 |
| 共享定义 | `driver/ramfan_ioctl.h` | IOCTL 与硬件常量（驱动/服务共用，来自 `LOG.md` 已确认事实） |
| 用户态服务 | `service/ramfan-service.c` | SCM 生命周期、`--once`（当前仅调用受资源门禁的 FEED_ONCE）、日志 |
| 构建 | `build.ps1` | 定位 VS/WDK，x64 Debug/Release |
| 安装 | `install-test.ps1` | 测试签名安装，Secure Boot 检查，显式警告 |
| 卸载 | `uninstall.ps1` | 停止/删除驱动与服务，回滚 |

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

## 安装（测试签名，开发测试版）

```powershell
# 管理员 PowerShell
pwsh -File install-test.ps1 -SignDriver -EnableTestSigning
# 若提示测试签名未开启且已执行 -EnableTestSigning：重启后重跑（去掉 -EnableTestSigning）
```

脚本行为：

1. **拒绝在 Secure Boot 开启时安装**（不绕过签名策略）；本机 Secure Boot 当前为关闭。
2. `-EnableTestSigning` 显式执行 `bcdedit /set testsigning on`（需重启，重启后重跑脚本）。
3. 用自签名测试证书签名 `ramfan.sys`（`-SignDriver`），非 PnP 驱动用
   `sc create RAMFanDriver type= kernel start= demand` 安装，无需 INF。
4. 注册用户态服务 `RAMFan`（`SERVICE_DEMAND_START`；常驻喂值仍留阶段 3）。

## 验证（阶段 2，资源模型解决前禁止执行）

按机主安排，项目大致完成后再安装驱动和执行写回验证。当前仅完成开发机编译，未执行 `install-test.ps1`、`sc start` 或任何真实 NCT 写入。

```powershell
sc start RAMFanDriver
& "$env:ProgramFiles\RAMFan\ramfan-service.exe" --once
```

预期日志：

```text
FEED_ONCE: status=4 max=0°C written=0°C readback=0°C
```

验收点：

- 当前资源门槛未满足时，`FEED_ONCE` 必须返回 status=4，且不访问 SMBus、不写 NCT。
- 资源模型解决并独立审查通过后，才验证“所有已映射 DIMM 读取成功后写入”；空槽 NACK 忽略，已装槽异常整轮失败。
- 资源模型解决并独立审查通过后，写入才允许严格限定为 page `0x0c` / reg `0x36`，写后读回一致并恢复原页；不写 page `0x09`。
- 对照历史 30°C/40°C 响应，确认 `pwm5/fan5` 单调变化；结束后重启确认 BIOS 曲线和温度源未改写。
- 测试期间停止 HWiNFO/OpenHardwareMonitor 等会访问 SMBus/NCT 端口的工具。

## 回滚

```powershell
pwsh -File uninstall.ps1
# 可选：bcdedit /set testsigning off（重启生效）
```

驱动停止/卸载不清除 NCT 最后一次写入值；不修改 BIOS、曲线或温度源。

## 已知风险与边界（阶段 2）

- **外部并发**：标准 SIO `0x2e/0x2f` 与自定义口 `0x295/0x296` 是主板级共享端口。
  实机测试前停止 HWiNFO/OpenHardwareMonitor 等会访问这些端口的工具；驱动内部串行队列
  只保证本驱动请求不交错，不能与其他程序自动原子化。
- **只读诊断基址**：驱动优先扫描 FCH SMBus BAR；失败时使用已确认的 `0xb00` 作为诊断路径。
  该值不是当前非 PnP 驱动的 translated resource，不能授权阶段 2 写回。
- **资源模型未解决（当前硬阻塞）**：非 PnP 驱动没有 `EvtDevicePrepareHardware` 或
  translated resource list。PCI/ACPI 证据不能授权本控制设备访问端口；独立 `PNP0C02` 的 NCT
  端口也不能由 PCI SMBus 证据推断。当前 `FEED_ONCE` 在任何硬件访问前返回
  `RAMFAN_FEED_HW_UNAVAILABLE`。后续应评估 AMD SMBus PCI upper-filter，并单独解决
  NCT `0x295/0x296` 及标准 SIO `0x2e/0x2f` 的资源/身份访问模型。
- **SMBus `0x04` 语义未充分区分**：驱动不再把它统一当作空槽 NACK；遇到该状态时
  映射失败或已映射槽整轮失败，不得用剩余 DIMM 降速。
- **失败保持旧值**：读取或写回失败时不写 0°C、不写猜测值；连续失败导致旧值过期是已知热安全风险。
- **测试签名版不伪装正式发布**：正式交付需 Microsoft Attestation/WHQL 等可信签名。

阶段 3 才启用 0.5 秒常驻喂值和自动启动；实机验证通过前不切换。
