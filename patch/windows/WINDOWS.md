# RamFan（Windows 补丁）— 受控非 PnP 访问

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` / reg `0x36`），使 `FAN5=MEM_FAN` 在 BIOS 数据源为“内存温度”时按 BIOS 曲线运行。

**当前阶段**（2026-09-06）：身份门禁、SPD 读取、FEED_ONCE、常驻 0.5s 喂值、方案 B（温度屏蔽 + 单槽重试 + 门禁缓存）均实机验证通过并经压测（45–46°C 稳定写回一致）。公测版（测试签名，一键安装 + 开机自启）已打包。

PnP 绑定（upper-filter / function-driver）已在实验 B/C 证伪并删除；驱动不接收 `EvtDeviceAdd`/translated resources。

- 驱动以普通内核服务加载（`sc create RAMFanPnP type= kernel`），创建非 PnP 控制设备 `\Device\RamFanVirtTemp`。
- `IOCTL_RAMFAN_QUERY_HW`：系统 PnP 枚举确认 PCI `DEV_790B` + NCT chip id（标准 SIO `0x2e/0x2f`）的拒绝式门禁。
- `IOCTL_RAMFAN_READ_DIMM_TEMP`：受控 SMBus 读取（`0xb00` 事务寄存器，命令 `0x31`，地址 `0x53..0x50`）。
- `IOCTL_RAMFAN_FEED_ONCE`：一次完成读取→校验→最高温→写回并读回校验（NCT page `0x0c` / reg `0x36`），页保存/恢复。方案 B：每轮全读 4 槽，有效温度 1..120°C（0°C 屏蔽），单槽瞬时垃圾/超时同轮重试一次；无任一有效温度槽才整轮不写。身份门禁 60s 缓存心跳。
- 服务：`--identity` 门禁、`--dimm` 读取实验、`--once` 单次写回、`--install`/`--uninstall`；SCM 常驻 0.5s FEED_ONCE 循环（AUTO_START 开机自启，依赖驱动服务 RAMFanPnP），连续失败退避、恢复失败独立 FATAL、日志节流、停止保留 NCT 值。

## 授权边界

- **身份依据**：主板型号、PCI `DEV_790B`（系统 PnP 枚举）、NCT chip id `0xd802`。用于拒绝式校验。
- **访问依据**：无已确认的 Windows 资源持有或独占授权。ACPI/PCI 证据不能推出端口访问授权。
- **实验批准**：2026-09-05 批准本目标机受控实验例外，逐级批准至常驻喂值并实机验证（见 `LOG.md` 对应日期条目）。
- **发布批准**：2026-09-06 定案——测试签名公测版为最终交付形态，无官方认证计划。

当前批准允许的驱动硬件访问：系统 PnP 枚举读 `Enum\PCI`（DEV_790B 存在性）、标准 SIO `0x2e/0x2f`（chip id 探针）、SMBus 基址 `0xb00` 事务寄存器（SPD word-read，命令 `0x31`）、NCT 自定义 SIO `0x295/0x296`（仅写 page `0x0c` / reg `0x36` 并读回校验，页保存/恢复）。不提供任意端口或任意寄存器 IOCTL。

## 前置条件

- Windows 11 x64（10.0.26200 已验证），管理员权限（安装脚本可自动 UAC 提权）。
- Visual Studio Build Tools 2022+（C++ 负载）+ Windows SDK + WDK 10。
- 目标机硬件：AMD FCH SMBus `VEN_1022&DEV_790B`；NCT chip id `0xd802`。
- testsigning 开启、测试证书 `RAMFanTestSign` 在 Machine 存储（`experiment-b-prep.ps1` 可准备）。
- 开发脚本用 PowerShell 7；分发脚本 `install.ps1`/`uninstall.ps1` 兼容 PowerShell 5.1。

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

## 发布打包（签名与组装）

```powershell
pwsh -NoProfile -File .\build-release.ps1
```

`build-release.ps1` 只签名并组装 `release/windows/`（gitignore）：驱动 `/ph` 页哈希签名、服务 exe 普通签名（签名对象是发布副本，源保持未签，重跑不累积签名），并连同根 README.md、LICENSE 一起复制，最后打印包内容清单。前置：先运行 `build.ps1 -Configuration Release`。

压缩由发布者手动完成：将 `release/windows/` 全部文件压为 `B850AIGA-RAM-FAN-PatchFix_Windows_TestSign.7z` 上传 GitHub release。

## 实机安装/验证（分发脚本，一键流程）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

第一遍运行：检测 Secure Boot（ON 则提示去 BIOS 关闭，脚本不能改）→ testsigning OFF 则自动 `bcdedit /set testsigning on` → 内存完整性运行/配置为启用则自动关闭（注册表）→ 测试证书缺失则自动导入同目录 RAMFanTestSign.cer → 有上述改动则提示重启。重启后再次运行同一脚本完成安装：复制驱动到 `%WinDir%\System32\drivers\ramfan.sys`，`sc create RAMFanPnP`（type= kernel, **auto**），复制服务到 `C:\ProgramData\RAMFan\ramfan-service.exe` 并 `--install`（AUTO_START + 依赖 RAMFanPnP），`sc start` 两者（0.5s 常驻喂值）。卸载脚本 `uninstall.ps1` 支持 `-RestoreSecurity` 自动还原 testsigning/内存完整性。

安装后验证：

```powershell
Get-Content C:\ProgramData\RAMFan\ramfan.log -Tail 20   # 观察喂值循环
C:\ProgramData\RAMFan\ramfan-service.exe --once          # 单次读→写回并读回校验
```

卸载：运行 `uninstall.ps1`（自动提权），停删 `RAMFan`/`RAMFanPnP` 服务、删驱动文件与 `ProgramData\RAMFan\ramfan-service.exe` 副本。

开发用实机脚本（机主或受权 agent 执行）：`identity-gate-prep.ps1` / `identity-gate-rollback.ps1`，与分发脚本并存，用于开发阶段逐级验证（身份门禁、SMBus 读取、写回）。历史结果见 `LOG.md` 2026-09-05 条目：QUERY_HW `SMBusBase=0x0b00 ChipId=d802 HwMatched=1`；`--dimm` 已装 `0x53/0x51` OK、空槽 `0x50/0x52` BUS_ERR；`--once` 6 轮 `status=0 written/readback 一致`；常驻 RUNNING 0.5s 每轮 FEED ok 写回一致。

身份未通过时检查：

- `ControllerFound=0`：系统 PnP 枚举中不存在 `VEN_1022&DEV_790B`（非目标机或该控制器被禁用）。
- `ChipId=ffff`：标准 SIO `0x2e/0x2f` 探针失败（被占用、无 NCT 或访问被拒）。
- 任何身份误判即回到 Linux 交付路线，不进入写回。

## 已知风险与边界

- 控制设备 ACL 仅 SYSTEM/管理员。
- 标准 SIO `0x2e/0x2f` 与自定义端口 `0x295/0x296` 是主板级共享端口；测试前停止 HWiNFO/OpenHardwareMonitor 等工具（`install.ps1` 会检测）。
- 写回只写 NCT page `0x0c`/reg `0x36`（Virtual_TEMP）；不写曲线/模式/温度源。无任一有效温度槽时才整轮不写（保留旧值）；连续失败旧值过期是已知热安全风险，不伪装成完整 fail-safe。
- 服务停止/驱动卸载不清除 NCT 最后写入值（保留到 NCT 复位/重启）。
- `sc stop` 1052：内核驱动一旦加载无法热卸载，靠重启释放文件（服务项删除后无自载风险）。
- 测试签名版不伪装官方签名；无官方认证计划，仅面向能接受安全降级的用户。

## 组件

| 组件           | 文件                                                              | 说明                                                                                                                   |
| -------------- | ----------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| 内核驱动       | `driver/ramfan.c`、`driver/hw.c`、`driver/identity_model.c` | 非 PnP 控制设备；门禁 + 读取 + 写回 + 温度屏蔽/重试                                                                    |
| 身份判定纯逻辑 | `driver/identity_model.h`、`driver/identity_model.c`          | 无 WDF 依赖：chip id 匹配判定，宿主自检使用                                                                            |
| 共享定义       | `driver/ramfan_ioctl.h`                                         | IOCTL、固定目标端口白名单、硬件常量                                                                                    |
| 用户态服务     | `service/ramfan-service.c`                                      | `--identity`/`--dimm`/`--once`；SCM 常驻 0.5s 循环（`--install`/`--uninstall`）                              |
| 构建           | `build.ps1`                                                     | 定位 VS/WDK，x64 Debug/Release                                                                                         |
| 发布打包       | `build-release.ps1`                                             | 签名发布副本 + 组装`release/windows/`（含根 README/LICENSE），打印包内容清单                                         |
| 分发安装       | `install.ps1`、`uninstall.ps1`                                | 自动提权；一键配置环境（testsigning/HVCI/证书）+ 重启续装 + AUTO_START 部署；卸载支持`-RestoreSecurity`；PS 5.1 兼容 |
| 安装说明       | `INSTALL.md`                                                    | 面向测试版用户的安装/卸载/风险说明                                                                                     |
| 测试证书       | `RAMFanTestSign.cer`                                            | 公钥证书（驱动加载失败 577 时导入）                                                                                    |
| 开发脚本       | `identity-gate-prep.ps1`、`identity-gate-rollback.ps1`        | 签名 + 加载驱动 + 运行检查（机主/受权 agent 执行）                                                                     |
| 历史工具       | `experiment-b-prep.ps1`、`experiment-b-rollback.ps1`          | 证书/签名/清理通用工具；`experiment-b-logs/` 结果不入库                                                              |
