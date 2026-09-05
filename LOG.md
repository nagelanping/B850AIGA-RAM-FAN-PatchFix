# LOG — MS-iCraft B850 AIGA 内存风扇曲线修复

## 当前状态

- **Linux 0.1 已归档**：实现和完整 Linux 工作记录位于 `archive/0.1/linux/`；发布包仍在 `release/linux/`。
- **当前工作目标**：实现 Windows KMDF 内核驱动 + Windows Service，持续向 NCT `Virtual_TEMP` 喂入 DIMM 温度。
- **当前阶段**：Windows 阶段 2代码草稿（FEED_ONCE 写回闭环）已完成，Debug/Release 构建通过；独立审查发现写回安全门槛阻塞项，尚未提交或进行实机安装/写回验证。
## 根因与目标链路

BIOS 将 `FAN5=MEM_FAN` 的温度源设置为 NCT6796D `Virtual_TEMP`（source `0x0a`）。该通道没有硬件数据，固件仅在 BIOS 打开内存风扇曲线页面时持续喂值；重启后 DXE/Setup 不再运行，温度值被清除，风扇回到约 941 rpm。

```text
南桥 SMBus → SPD5118 DIMM 温度 → NCT Virtual_TEMP → BIOS Smart Fan → FAN5
```

Windows 版本的服务每 0.5 秒请求驱动完成一轮读写。读取不完整或写回失败时跳过本轮，保持最后一次成功值；不写 0°C 或猜测值。

## 已确认硬件事实

| 项目              | 已确认值                                                       |
| ----------------- | -------------------------------------------------------------- |
| 主板 / BIOS       | MAXSUN MS-iCraft B850 AIGA；BIOS E1.6D                         |
| NCT               | 实测 chip id`0xd802`，NCT6796D-S / NCT6799D 兼容系列         |
| NCT 固件端口      | index`0x295`、data `0x296`                                 |
| NCT 标准 SIO 端口 | `0x2e/0x2f`；Linux 驱动基址 `0x290`                        |
| 内存风扇          | `FAN5=MEM_FAN`；bank/page `0x09`                           |
| 内存风扇源        | page`0x09`、reg `0x00` = `0x0a`（Virtual_TEMP）          |
| Virtual_TEMP 写入 | page`0x0c`、reg `0x36`                                     |
| 写入编码          | 摄氏度整数，`0x1e` = 30°C                                   |
| DIMM 温度总线     | FCH/南桥 SMBus 基址`0xb00`                                   |
| SPD 设备          | 7-bit 地址`0x53, 0x52, 0x51, 0x50`；命令 `0x31`；word read |
| 温度换算          | `scaled=(raw << 3) >> 5`；`celsius=(scaled * 25) / 100`    |
| 轮询周期          | 0.5 秒                                                         |

NCT 页选择必须保留高 4 位：

```text
outb(0x4e, 0x295)
v = inb(0x296)
outb((v & 0xf0) | page, 0x296)
```

实测响应：写 30°C → `pwm5=76`、约 1031 rpm；写 40°C → `pwm5=101`、约 1326 rpm。

## SMBus 参考

控制器寄存器（基址 `0xb00`）：

```text
+0x00 HST_STS
+0x02 HST_CNT
+0x03 HST_CMD
+0x04 HST_ADD
+0x05 HST_DAT0
+0x06 HST_DAT1
```

一次 SPD word-read：清状态 → 写 `(addr7 << 1) | 1` → 写命令 `0x31` → 启动 `0x4c` → 等待 BUSY 清除 → 检查状态 → 读两个字节。实测 `HST_STS=0x02` 可以是成功完成标志，不能将 bit1 当作错误；`0x04` 为无设备/CRC 样式错误。Windows 驱动必须设置超时，并拒绝状态异常、raw 异常和 `0..120°C` 范围外的结果。

固件逆向确认：

- M351 SMBus HST 底层约在 `0x223e4`；各 DIMM 封装位于 `0x22498`、`0x224f0`、`0x22554`、`0x225b8`、`0x226c0`、`0x227c8` 等。
- `SkSmartFanProtocol` 的温度采样/写回链路位于约 `0x183c`；每次写回前都会选择 NCT page `0x0c`。
- `M351` 的内存温度采集/打印主逻辑约为 `0xf0000–0xf0bxx`。
- NCT 自身 `SMBUSMASTER` 读取 DIMM 的路径已排除；NCT 硬件温度源没有可用 DDR 温度。

## Windows 开发决策

- 采用 **KMDF 驱动 + Windows Service**，不把 WinRing0、InpOut32 等第三方驱动作为正式依赖。
- 驱动内部完成端口 I/O、SMBus 事务、温度换算和 NCT 写回；服务只调用最小的 `IOCTL_RAM_FAN_FEED_ONCE`，不暴露裸端口操作。
- 驱动首轮必须从 Windows PCI/FCH 资源确认 SMBus 基址，不能只硬编码 `0xb00`；同时检查 FCH 控制器和 NCT chip id，硬件不匹配则拒绝工作。
- 测试签名、Secure Boot、驱动安装和回滚是 Windows 交付边界；测试版本不得伪装为正式签名发布。

## Windows 待办

1. ✅ 记录 Windows 环境（见下方 2026-09-04 Windows 环境记录）。
2. ✅ 准备 VS + SDK + WDK（VS 2026 Community 18.9 / SDK 10.0.26100 已装；WDK 10.0.26100 安装中）；阶段 1 工程已建。
3. ✅ 阶段 1 代码：驱动加载/卸载、硬件识别、设备句柄、只读 SMBus IOCTL；待构建与实机验证（禁止写 NCT）。
4. 阶段 2资源门槛：评估 AMD SMBus PCI upper-filter，确认 translated SMBus 资源；单独确认 `PNP0C02` 的 NCT 端口及 `0x2e/0x2f` 访问模型；完成后再恢复 FEED_ONCE 闭环。
5. 阶段 3：启用 0.5 秒常驻服务，验证服务重启、睡眠恢复和系统重启后自动恢复。
6. 阶段 4：完成空槽、单 DIMM 失败、全失败、超时、读回不一致、卸载回滚和内存加压测试。
7. 阶段 1 代码完成后交子代理独立审查；机主实机验证结果、签名方式、版本和风险写回本文件。

## 风险与禁止事项

- 不同时运行多个会写 `0x295/0x296` 的硬件监控/补丁程序；驱动内部锁不能解决外部程序竞争。
- 服务停止不会清除 NCT 最后一次写入值；不实现未经验证的退出“安全温度”写入。
- 不修改 page `0x09` 的曲线、模式、温度源或其他寄存器。
- 不修改 BIOS、UEFI 变量或 SMM；刷写 BIOS 必须另行批准并备份、校验。
- 如果 Windows 无法在可信签名和可接受安全边界内访问端口，停止扩展实现并记录原因，不绕过驱动签名策略交付。

## 2026-09-04 Windows 环境记录（阶段 0 实机查询）

- **OS**：Windows 11 专业工作站版 10.0.26200 (build 26200)，x64。
- **Secure Boot**：当前 **False**（与 WORKFLOW 默认前提相反，已记录；测试签名驱动可用于开发测试，无需改 BIOS）。
- **内存**：2×16GB DDR5-5600（DIMM1 P0 CHANNEL A / B），对应 SPD 7-bit 地址 `0x53/0x52` 两个已装槽。
- **SMBus 控制器**：`PCI\VEN_1022&DEV_790B`（AMD FCH SMBus，rev 71），驱动 oem14.inf，状态 OK。
- **SMBus 基址证据**：ACPI `PNP0C02\700` 声明 IO `0xb00-0xb0f`（与 Linux 实测 `0xb00` 一致）。
- **NCT 端口证据**：`0x295/0x296` 落在 ACPI `PNP0C02\0` 声明的 `0x290-0x29F` 范围内，访问模型有 ACPI 背书。
- **驱动签名策略**：`CI\Policy` V&R 键不存在；bcdedit 无 testsigning/integrity 输出（未开启测试签名）。
- **工具链**：VS 2026 Community 18.9（MSVC 14.51）+ Windows SDK 10.0.26100 已装；WDK 10.0.26100 安装中；无 HWiNFO 类进程在跑。

## 2026-09-04 Windows 阶段 1 骨架（patch/windows/）

- 采用**非 PnP KMDF 驱动 + SCM 直装**：微软文档确认非 PnP KMDF 驱动在 Win10/11 无需 INF/co-installer，直接 `sc create`；驱动必须提供 `EvtDriverUnload` 并手动创建控制设备（`WdfDeviceInitAllocate` + `WdfDeviceCreate`）。已删 `ramfan.inf`。
- `driver/ramfan_ioctl.h`：驱动/服务共享常量与 IOCTL（QUERY_HW / READ_DIMM_TEMP），不含内核头依赖。
- `driver/hw.c`：PCI 扫描 FCH SMBus BAR（slot 编码 `device<<16|func<<8` 已验证）回退 ACPI `0xb00`；NCT chip id 走标准 SIO `0x2e/0x2f` 解锁读取后 `0xaa` 锁定；SMBus word-read 100ms 超时，`0x04` 判无设备、`0x02` 不判失败。
- `driver/ramfan.c`：非 PnP 设备 `\Device\RamFanVirtTemp`，SDDL 限 SYSTEM/管理员；串行队列；只读 IOCTL。
- `service/ramfan-service.c`：SCM 生命周期、`--once`（0=成功/1=硬件失败/2=参数错误）、`C:\ProgramData\RAMFan\ramfan.log`。
- `build.ps1` / `install-test.ps1` / `uninstall.ps1` / `WINDOWS.md`：构建（vswhere+msbuild）、测试签名安装（Secure Boot 开启即拒绝）、回滚、文档。
- **待办**：WDK 安装完成后本机构建验证；阶段 1 交子代理审查。

## 2026-09-04 阶段 1 子代理独立审查（已修复）

- 审查结论：结构/硬件事实/安全边界/阶段范围一致，**有条件通过**；验收前必须修复 S1。
- **[严重] S1** `hw.c` 超时失效：`KeQueryPerformanceCounter` 返回计数器、频率为出参，原代码把 `start`/`now` 当出参接收频率，差值恒 0，BUSY 挂起时无限轮询阻塞服务 → 改为 `start = KeQueryPerformanceCounter(&freq)`、循环内 `now = KeQueryPerformanceCounter(NULL)`。
- **[中等] M1** `WDF_FILEOBJECT_CONFIG_INIT` 参数错位：`RamFanEvtFileClose` 被放进 EvtFileCleanup 槽（WDK 1.33 参数序为 Create/Close/Cleanup）→ 移到第 2 槽。
- **[中等] M2** 温度 UCHAR 截断先于 0..120 校验，越界值可折叠进有效区间 → `RamFanCelsiusFromRaw` 返回 ULONG，校验在截断前，合法后显式 `(UCHAR)` 转换。
- **[中等] M3** 测试证书未导入 LocalMachine 信任存储（内核 CI 校验不可见 CurrentUser）→ 脚本导出证书并导入 `TrustedPublisher` + `Root`。
- **[轻微]** bcdedit 无匹配行时 `.ToString()` 抛异常 → 加 null 防御；PCI 全量重扫/返回值校验等留阶段 2/3。
- 修复后 Debug/Release 双配置构建通过：`ramfan.sys` 16.5KB、`ramfan-service.exe` 44.5KB。

## 2026-09-04 Windows 工作流轻量审查

- 结论：KMDF 驱动 + Windows Service 的方向可行，先只读、再一次性写回、最后常驻服务的顺序合理。
- 已在 `WORKFLOW.md` 补充阻塞前置：驱动 PnP/resource model、空槽 NACK 与已安装 DIMM 故障的区分、SMBus 超时清理、NCT 页恢复、外部并发限制及正式签名门槛。
- 旧值保持策略仍不写未经验证的安全温度；连续失败导致旧值过期属于明确热安全风险，须在目标机测试和发布说明中保留。


## 2026-09-04 AGENTS.md 双系统维护

- 根目录 `AGENTS.md` 已从单一混合约束维护为 Linux / Windows 两部分：Linux 维护边界、Windows KMDF + Service 开发边界、阶段门槛、实机权限和共享硬件访问风险。
- 保留既有项目规则和关键硬件事实，未按归档 `LOG.md` 的方式删减历史；Linux 归档继续保留完整记录。
- 子代理轻量审查通过：未发现与 `WORKFLOW.md` 或已确认硬件事实冲突；已同步温度范围、DIMM 地址映射/失败语义、Linux 部分验收状态和 Windows 服务启动策略。


## 2026-09-04 Windows 阶段 2草稿与独立审查

- 曾实现过：`IOCTL_RAMFAN_FEED_ONCE`、首轮 DIMM 映射、最高温度选择、NCT page `0x0c`/reg `0x36` 写入/读回及原页恢复；该写回草稿因资源门槛已移除。服务 `--once` 接口保留但当前只返回资源未授权。服务常驻仍只做阶段 1只读检查，未启用 0.5 秒写回。
- 开发机构建：驱动和服务 Debug/Release 均通过；未执行安装、驱动加载、真实端口访问或 NCT 写回。
- 子代理审查结论：**不通过，暂不提交阶段 2写回代码**。
- 阻塞项：当前非 PnP 驱动没有 translated resource list 运行时确认；`0x04` 无法区分空槽 NACK 与 CRC 样式错误，首轮映射可能误判已装 DIMM；状态位分类仍需收紧。
- 已修复的审查项：SMBus 超时后清状态并有限轮询 BUSY；首轮失败日志不再使用伪造的 `status=0` 槽位；服务启动只读检查有限重试；`--once/--install/--uninstall` 互斥；测试签名状态保存/恢复，Secure Boot 查询失败时中止。
- 下一步：先解决上述写回安全门槛并再次独立审查；项目大致完成后再由机主执行实机安装、失败路径、NCT 读回及风扇响应验证。

## 2026-09-04 阶段 2安全阻断（继续）

- `FEED_ONCE` 已改为资源门禁：当前非 PnP 控制设备没有 translated resource list，调用在任何 SMBus/NCT 访问前返回 `RAMFAN_FEED_HW_UNAVAILABLE`，不写 NCT。
- 移除未获资源授权的 NCT 写回实现；PCI BAR/ACPI `_CRS` 仍只作为诊断证据，不能授权当前控制设备访问独立 `PNP0C02` 的 NCT 端口。
- SMBus `HST_STS=0x04` 不再统一转换为 `STATUS_DEVICE_NOT_CONNECTED`；该状态可能是空槽 NACK 或 CRC/总线异常，当前按不确定数据错误处理，不能建立空槽映射或使用剩余样本降速。
- 资源架构建议采用两个独立 PnP 设备：AMD SMBus PCI upper-filter 持有 SMBus translated resource；经确认可绑定的 `PNP0C02` 驱动单独持有 NCT translated resource。不能把两个资源合并，也不能由 PCI 资源授权 NCT 端口。
- 阶段 2仍不提交；下一步是评估 AMD SMBus PCI upper-filter，并单独确认 NCT `0x295/0x296` 与标准 SIO `0x2e/0x2f` 的合法资源/身份访问模型。

## 2026-09-04 实机 PnP 资源收集结果

- 目标机只读脚本已执行成功；未安装驱动、未启动服务、未访问 I/O port。
- `PCI\VEN_1022&DEV_790B&SUBSYS_07606688&REV_71\3&11583659&0&A0` 显示 AMD SMBUS，但 `pnputil /resources` 未列出 I/O resource。
- `ACPI\PNP0C02\700` 显示 I/O `0x0010-0x001F`、`0x0022-0x003F` 等范围，并明确包含 `0x0B00-0x0B0F`；因此当前不能假设 PCI upper-filter 可获得 SMBus `0xb00`。
- `ACPI\PNP0C02\0` 显示 I/O `0x0290-0x029F` 和 `0x0200-0x023F`；后者覆盖标准 SIO `0x2e/0x2f`。该实例使用 `machine.inf`，状态为 PnP Stopped / 查询状态 OK；能否安全挂 upper-filter、是否存在共享访问影响仍未确认。
- `DEVPKEY_Device_ResourceList` 与 `DEVPKEY_Device_ResourceListTranslated` 的 PowerShell 属性查询返回“无效的参数”；`pnputil /resources` 提供了当前可读的端口范围证据。
- 资源模型方向已从“PCI SMBus upper-filter + 独立 PNP0C02 NCT”修正为优先研究 `PNP0C02` 资源设备 upper-filter，并分别按 `0xb00` 与 `0x290` 资源实例建立上下文。

## 2026-09-04 PnP 资源识别骨架进度

- 已将驱动初始化改为 KMDF PnP upper-filter 形态：`DriverEntry` 注册 `EvtDeviceAdd`，PnP 设备在 `EvtDevicePrepareHardware` 只解析 translated `CmResourceTypePort`，`EvtDeviceReleaseHardware` 清空上下文。
- 当前骨架识别 `0x0b00-0x0b0f`、`0x0290-0x029f` 和 `0x0200-0x023f`，但还没有把两个资源实例连接到控制设备全局状态；`FEED_ONCE`、`QUERY_HW`、`READ_DIMM_TEMP` 均不访问硬件。
- `hw.c` 已从工程和调用链断开；没有 PCI 扫描、固定基址回退、SMBus/NCT 端口访问。
- 新增 `ramfan.inf` 仅作为开发骨架，`install-test.ps1` 已主动拒绝执行，避免修改 `PNP0C02` 设备栈。当前禁止安装、加载、服务启动和 `--once`。
- Debug/Release x64 驱动与服务构建通过；构建输出明确提示安装流程暂停。
- 独立审查未通过，阻塞项包括：INF 的 upper-filter/AddService 语义与通配绑定范围、资源上下文尚未接入控制设备、catalog/签名流程、PnP 卸载回滚、旧服务/文档残留。
- 资源范围判断已补充负地址、地址溢出和 64 位到 32 位截断防护；目标端口上下文保存为固定目标范围，不把聚合 descriptor 起始地址误作基址。
- 当前骨架新增驱动级 wait-lock 和 SMBus/NCT 两个角色槽位：PrepareHardware 只登记唯一角色，ReleaseHardware 注销；重复角色标记冲突。该状态尚未用于硬件访问，FEED_ONCE 仍无条件阻断。
- INF 已移除错误的关联功能驱动标志（`AddService` 不再使用 `SPSVCINST_ASSOCSERVICE`）；由于 catalog/签名和设备栈安装流程仍未完成，安装与卸载脚本均主动拒绝执行。
- 用户态服务的 `--install/--uninstall` 入口也已在骨架阶段禁用，避免绕过 PowerShell 安全门直接修改 SCM；常规服务模式和 `--once` 仍未用于实机。
- 下一步先修正上述软件架构和安装包问题，再安排只读 PnP 加载验证；在此之前不需要机主执行新的实机指令。
## 参考资料

- `WORKFLOW.md`：Windows 当前实施和验收流程。
- `archive/0.1/linux/`：Linux 0.1 的完整 LOG、WORKFLOW 和实现背景。
- `ref/scripts/exp1_virtemp_probe.py`：Virtual_TEMP 目标寄存器与响应实验。
- `ref/scripts/exp2_smbus_probe.py`：南桥 SMBus DIMM 读取实验。
- `ref/scripts/sio_probe.py`、`sio_probe2.py`、`sio_dump.txt`：NCT/SIO 探测。
- `ref/disasm/`、`ref/mods/`：固件逆向材料。
- `ref/kernel/nct6775-core.c`：NCT 温度源和寄存器参考。
