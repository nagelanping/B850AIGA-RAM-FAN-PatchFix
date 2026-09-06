# ramfan（Windows 补丁）— 受控非 PnP 访问（身份门禁 + 受控读取 + 常驻喂值）

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为“内存温度”时按 BIOS 曲线运行。

**当前阶段**（2026-09-06）：身份门禁、SMBus 读取、FEED_ONCE、常驻 0.5s 喂值、方案 B（温度屏蔽+单槽重试+门禁缓存）均实机验证通过并经压测（测试签名版）。
PnP 绑定（upper-filter / function-driver）已在实验 B/C 证伪并删除；驱动不再接收 `EvtDeviceAdd`/translated resources。

- 驱动以普通内核服务加载（`sc create RAMFanPnP type= kernel`），创建非 PnP 控制设备 `\Device\RamFanVirtTemp`。
- `IOCTL_RAMFAN_QUERY_HW`：系统 PnP 枚举确认 PCI `DEV_790B` + NCT chip id（标准 SIO `0x2e/0x2f`）的拒绝式门禁。
- `IOCTL_RAMFAN_READ_DIMM_TEMP`：受控 SMBus 读取（`0xb00` 事务寄存器，命令 `0x31`，地址 `0x53..0x50`）。
- `IOCTL_RAMFAN_FEED_ONCE`：读取→校验→最高温→NCT page `0x0c`/reg `0x36` 写回并读回校验；页保存/恢复。方案 B：每轮全读 4 槽，有效温度 1..120°C（0°C 屏蔽），单槽瞬时垃圾/超时同轮重试一次；无任一 OK 槽才整轮不写。身份门禁 60s 缓存心跳。
- 服务：`--identity` 门禁、`--dimm` 读取实验、`--once` 单次写回；SCM 常驻 0.5s FEED_ONCE 循环（`--install`/`--uninstall`，DEMAND_START），连续失败退避、恢复失败独立 FATAL、日志节流、停止保留 NCT 值。
## 组件

| 组件 | 文件 | 说明 |
| ---- | ---- | ---- |
| 内核驱动 | `driver/ramfan.c`、`driver/hw.c`、`driver/identity_model.c` | 非 PnP 控制设备；门禁 + 读取 + 写回 + 温度屏蔽/重试 |
| 身份判定纯逻辑 | `driver/identity_model.h`、`driver/identity_model.c` | 无 WDF 依赖：chip id 匹配判定，宿主自检使用 |
| 共享定义 | `driver/ramfan_ioctl.h` | IOCTL、固定目标端口白名单、硬件常量 |
| 用户态服务 | `service/ramfan-service.c` | `--identity`/`--dimm`/`--once`；SCM 常驻 0.5s 循环（`--install`/`--uninstall`） |
| 构建 | `build.ps1` | 定位 VS/WDK，x64 Debug/Release |
| 实机加载/验证 | `identity-gate-prep.ps1`、`identity-gate-rollback.ps1` | 签名 + 加载驱动 + 运行检查（机主/受权 agent 执行） |
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

## 实机验证（管理员；机主或受权 agent 执行）

```powershell
pwsh -File .\patch\windows\identity-gate-prep.ps1 -Configuration Release
```

脚本会：检查环境（管理员、Secure Boot False、testsigning on、无端口监控进程）→ 用测试证书
以 `/ph` 签名驱动 → 复制到 `%WinDir%\System32\drivers\ramfan.sys` → `sc create RAMFanPnP`
（type= kernel, demand）→ `sc start` → 运行 `ramfan-service.exe --identity`。

§5.2 第 1 步身份门禁预期（日志在 `experiment-b-logs\identity-gate.log`）：
`QUERY_HW: SMBusBase=0x0b00 ChipId=d802 ControllerFound=1 ChipIdValid=1 HwMatched=1`（已实机验证通过）。

§5.2 第 2 步受控 SMBus 读取实验（驱动已加载时）：
```powershell
ramfan-service.exe --dimm
```
输出 4 槽（0x53/0x52/0x51/0x50）逐槽 Status/Raw/Celsius/HstSts。实机已确认：已装 0x53/0x51
OK 且有温度（~30-36°C，hst=0x02），空槽 0x50/0x52 呈 BUS_ERR+HstSts 0x06（无设备，0x04 位）。
§5.2 第 3 步单次写回与常驻喂值（驱动已加载时）：

```powershell
ramfan-service.exe --once         # 单次读→写回并读回校验
ramfan-service.exe --install       # SCM 安装（DEMAND_START）
sc start RAMFan                    # 启动 0.5s 常驻喂值循环
Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20  # 观察循环
sc stop RAMFan; ramfan-service.exe --uninstall  # 停止并卸载
```

实机已确认（2026-09-05）：`--once` `status=0 written/readback 一致`；常驻 RUNNING 下 0.5s 每轮
`FEED ok: max=3x written=3x readback=3x` 随 DIMM 温度动态波动；服务停止保留 NCT 值、重启平滑恢复。

若身份未通过，检查：
- `ControllerFound=0`：系统 PnP 枚举中不存在 `VEN_1022&DEV_790B`（非目标机或该控制器被禁用）。
- `ChipId=ffff`：标准 SIO `0x2e/0x2f` 探针失败（被占用、无 NCT 或访问被拒）。
- 任何身份误判即回到 Linux 交付路线，不进入写回。

验证后回滚：

```powershell
pwsh -File .\patch\windows\identity-gate-rollback.ps1
```


## 待机主验收（测试版本）

- 动态温升联动：常驻运行时温度变化 → 转速响应（观察内存风扇）。
- 睡眠恢复与系统重启后恢复：当前 DEMAND_START 需 `sc start RAMFan`；验收通过后改为自动启动（届时再经批准）。
- 正式发布仍待可信签名（Attestation/WHQL）与独立访问依据；测试签名版不得绕过签名策略交付。

## 已知风险与边界

- 控制设备 ACL 仅 SYSTEM/管理员；`--identity` 由管理员运行。
- 标准 SIO `0x2e/0x2f` 是主板级共享端口；测试前停止 HWiNFO/OpenHardwareMonitor 等工具。
- 写回只写 NCT page `0x0c`/reg `0x36`（Virtual_TEMP）；不写曲线/模式/温度源。无任一有效温度槽时才整轮不写（保留旧值）；连续失败旧值过期是已知热安全风险，不伪装成完整 fail-safe。
- 测试签名版不伪装正式发布；正式交付需 Microsoft Attestation/WHQL 等可信签名。
- 服务停止/驱动卸载不清除 NCT 最后写入值（保留到 NCT 复位/重启）。
- `sc stop` 1052：内核驱动一旦加载无法热卸载，靠重启释放文件（服务项删除后无自载风险）。
