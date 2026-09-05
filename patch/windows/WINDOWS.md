# ramfan（Windows 补丁）— 受控非 PnP 身份门禁阶段

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为“内存温度”时按 BIOS 曲线运行。

**当前阶段**（2026-09-05，机主批准受控非 PnP 访问模型，见根目录 `AGENTS.md`“授权边界”）：只读身份门禁。
PnP 绑定（upper-filter / function-driver）已在实验 B/C 证伪并删除；驱动不再接收 `EvtDeviceAdd`/translated resources。

- 驱动以普通内核服务加载（`sc create RAMFanPnP type= kernel`），创建非 PnP 控制设备 `\Device\RamFanVirtTemp`。
- 只实现 `IOCTL_RAMFAN_QUERY_HW`：系统 PnP 枚举确认 PCI `DEV_790B` 存在（读 `Enum\PCI` 注册表，pci.sys 权威枚举）+ NCT chip id（标准 SIO `0x2e/0x2f`）的拒绝式身份门禁。实机证实 `HalGetBusDataByOffset` 在此平台读不到 PCI 配置空间，故不直接扫描 PCI。
- `READ_DIMM_TEMP`、`FEED_ONCE` 保持阻断：不访问 SMBus 事务寄存器、不写 NCT `0x295/0x296`、不写 page `0x0c`。
- 服务 `--identity` 只做身份检查；SCM 常驻喂值留到后续阶段（需单独批准）。

## 组件

| 组件 | 文件 | 说明 |
| ---- | ---- | ---- |
| 内核驱动 | `driver/ramfan.c`、`driver/hw.c`、`driver/identity_model.c` | 非 PnP 控制设备 + 只读身份门禁 |
| 身份判定纯逻辑 | `driver/identity_model.h`、`driver/identity_model.c` | 无 WDF 依赖：chip id 匹配判定，宿主自检使用 |
| 共享定义 | `driver/ramfan_ioctl.h` | IOCTL、固定目标端口白名单、硬件常量 |
| 用户态服务 | `service/ramfan-service.c` | `--identity` 只读检查；`--once`/`--install`/`--uninstall` 仍拒绝 |
| 构建 | `build.ps1` | 定位 VS/WDK，x64 Debug/Release |
| 实机验证 | `identity-gate-prep.ps1` | 签名 + 加载驱动 + 运行 `--identity`（机主执行） |
| 回滚 | `identity-gate-rollback.ps1` | 停止/删除 `RAMFanPnP`、删除驱动文件（机主执行） |
| 历史工具 | `experiment-b-prep.ps1`、`experiment-b-rollback.ps1` | 证书/签名/清理通用工具；`experiment-b-logs/` 结果不入库 |

## 授权边界（四件事分开记录）

- **身份依据**：主板型号、PCI `DEV_790B`（系统 PnP 枚举）、ACPI `PNP0C02` 声明、NCT chip id `0xd802`。用于拒绝式校验。
- **访问依据**：当前无已确认的 Windows 资源持有或独占授权。ACPI/PCI 证据不能推出端口访问授权。
- **实验批准**：机主 2026-09-05 批准本目标机受控实验例外，允许只读身份探针（`Enum\PCI` 注册表 + `0x2e/0x2f`）。
- **发布批准**：默认不具备。正式交付需可信签名与独立访问依据。

本阶段驱动允许的硬件访问仅限：系统 PnP 枚举（读 `Enum\PCI` 注册表确认 DEV_790B 存在）与标准 SIO `0x2e/0x2f`
解锁→读 chip id→锁定。不做 SMBus 事务、不写 NCT 自定义端口。

## 前置条件

- Windows 11 x64（10.0.26200 已验证），管理员 PowerShell。
- Visual Studio Build Tools 2022+（C++ 负载）+ Windows SDK + WDK 10。
- 目标机硬件：AMD FCH SMBus `VEN_1022&DEV_790B`；NCT chip id `0xd802`。
- testsigning 开启、测试证书 `RAMFanTestSign` 在 Machine 存储（`experiment-b-prep.ps1` 可准备）。

## 构建

```powershell
pwsh -File build.ps1 -Configuration Debug
pwsh -File build.ps1 -Configuration Release
```

产物：`driver\x64\<Config>\ramfan.sys`、`service\x64\<Config>\ramfan-service.exe`。

## 纯逻辑自检（本机，无硬件访问）

```powershell
pwsh -NoProfile -File .\test-identity-model.ps1
pwsh -NoProfile -File .\test-smbus-model.ps1
```

- `test-identity-model.ps1` 编译并运行驱动实际使用的 `identity_model.c`，覆盖 chip id 匹配/失败/非预期/控制器缺失判定。
- `test-smbus-model.ps1` 验证 HST 状态分类、温度换算、温度范围和 SPD 地址顺序。
两个脚本不加载驱动、不打开设备、不访问端口。

## 实机只读身份门禁（机主执行，管理员）

```powershell
pwsh -File .\patch\windows\identity-gate-prep.ps1 -Configuration Release
```

脚本会：检查环境（管理员、Secure Boot False、testsigning on、无端口监控进程）→ 用测试证书
以 `/ph` 签名驱动 → 复制到 `%WinDir%\System32\drivers\ramfan.sys` → `sc create RAMFanPnP`
（type= kernel, demand）→ `sc start` → 运行 `ramfan-service.exe --identity`。

预期结果（日志在 `experiment-b-logs\identity-gate.log`）：
`QUERY_HW: SMBusBase=0x... ChipId=d802 ControllerFound=1 ChipIdValid=1 HwMatched=1`。

若身份未通过，检查：
- `ControllerFound=0`：系统 PnP 枚举中不存在 `VEN_1022&DEV_790B`（非目标机或该控制器被禁用）。
- `ChipId=ffff`：标准 SIO `0x2e/0x2f` 探针失败（被占用、无 NCT 或访问被拒）。
- 任何身份误判即回到 Linux 交付路线，不进入写回。

验证后回滚：

```powershell
pwsh -File .\patch\windows\identity-gate-rollback.ps1
```

## 下一阶段门槛（暂不执行）

- §5.2 第 2 步受控 SMBus 试验（读 DIMM）：涉及事务寄存器写入，需机主单独批准。
- 之后才恢复 `FEED_ONCE` 单次写回、动态闭环、常驻服务与失败试验。
- 每次从只读进入写回前，须独立子代理审查与实机证据记录。

## 已知风险与边界

- 控制设备 ACL 仅 SYSTEM/管理员；`--identity` 由管理员运行。
- 标准 SIO `0x2e/0x2f` 是主板级共享端口；测试前停止 HWiNFO/OpenHardwareMonitor 等工具。
- 本阶段不写任何值，无热安全风险；后续写回步骤才引入旧值保持策略。
- 测试签名版不伪装正式发布；正式交付需 Microsoft Attestation/WHQL 等可信签名。
- 驱动卸载不清除任何 NCT 状态（本阶段也没有写入）。
