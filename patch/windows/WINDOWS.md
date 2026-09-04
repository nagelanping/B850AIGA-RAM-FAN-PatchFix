# ramfan（Windows 补丁）— 阶段 1：只读骨架

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为"内存温度"时按 BIOS 曲线运行。

**当前阶段（阶段 1）**：KMDF 驱动骨架 + 最小服务，只读 SMBus DIMM 温度，
**禁止写 NCT**。完整喂值闭环在阶段 2，常驻服务在阶段 3（见 `WORKFLOW.md`）。

## 组件

| 组件 | 文件 | 说明 |
| ---- | ---- | ---- |
| 内核驱动 | `driver/ramfan.c` `driver/hw.c` | 非 PnP KMDF；`\Device\RamFanVirtTemp`；PCI 扫描 SMBus 基址 + SIO chip id + 只读 SMBus word read |
| 共享定义 | `driver/ramfan_ioctl.h` | IOCTL 与硬件常量（驱动/服务共用，来自 `LOG.md` 已确认事实） |
| 用户态服务 | `service/ramfan-service.c` | SCM 生命周期、`--once`、日志 |
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
4. 注册用户态服务 `RAMFan`（`SERVICE_DEMAND_START`，阶段 1 不自动启动）。

## 验证（阶段 1 验收点）

```powershell
sc start RAMFanDriver
& "$env:ProgramFiles\RAMFan\ramfan-service.exe" --once
```

期望输出（对照 `LOG.md` 已确认值）：

```
QUERY_HW: SMBusBase=0xb00 ChipId=d802 HwMatched=1
  DIMM 0x53: status=0 raw=... temp=...
  DIMM 0x52: status=0 raw=... temp=...
  ...（0x51/0x50 空槽应为 status=1 NACK，不算失败）
READ_DIMM_TEMP: AnySuccess=1 MaxCelsius=...
```

验收点：

- `HwMatched=1`：SMBus 基址有效且 chip id `d8 02` 匹配；不匹配时拒绝工作。
- 两根已装 DIMM（本机 2×16GB）读到合理温度（0..120°C），与 HWiNFO/BIOS 对照在合理误差内。
- 空槽 `NACK` 不算失败；已装槽超时/总线错误/非法数据会导致该槽标记失败（阶段 1 仅报告，阶段 2 决定整轮语义）。
- 驱动加载/卸载干净：`sc query RAMFanDriver`、`sc stop RAMFanDriver`、`sc delete RAMFanDriver`。
- **本阶段不写 NCT**：`0x295/0x296` 除 chip id 探测（走标准 SIO `0x2e/0x2f`，读后 `0xaa` 锁定）外不被写入。

## 回滚

```powershell
pwsh -File uninstall.ps1
# 可选：bcdedit /set testsigning off（重启生效）
```

驱动停止/卸载不清除 NCT 任何值（阶段 1 未写 NCT）；不修改 BIOS、曲线、温度源。

## 已知风险与边界（阶段 1）

- **外部并发**：标准 SIO `0x2e/0x2f` 与自定义口 `0x295/0x296` 是主板级共享端口。
  测试时停止 HWiNFO/OpenHardwareMonitor 等会访问这些端口的工具；驱动内部串行队列
  只保证本驱动请求不交错，不能与其他程序自动原子化。
- **PCI 扫描回退**：驱动优先从 PCI 配置空间读取 FCH SMBus BAR；失败时回退到实机
  已确认的 ACPI 固定基址 `0xb00`（记录在案，非无依据硬编码）。
- **非 PnP 驱动无 PnP 资源回调**：`WdfDriverInitNonPnpDriver` 下设备在
  `DriverEntry` 手动创建；阶段 2 若改绑 PnP 设备，再迁移到
  `EvtDevicePrepareHardware` 资源模型。
- **测试签名版不伪装正式发布**：正式交付需 Microsoft Attestation/WHQL 等可信签名。

## 阶段 2 入口（下一个任务）

在 `hw.c` 增加 NCT 页选择（`0x295/0x296`，保留高 4 位）与 `page 0x0c/reg 0x36`
写入+读回校验；在 `ramfan.c` 增加 `IOCTL_RAMFAN_FEED_ONCE`；服务实现 `--once`
完整"读取→校验→取最高→写回"闭环；目标机对照 30°C/40°C 历史响应验证
`pwm5/fan5` 变化。写回前必须完成 WORKFLOW.md §3.2 的端口资源/访问模型确认。
