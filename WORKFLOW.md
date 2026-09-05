# Windows / Linux 内存风扇修复工作流

## 1. 唯一目标

修复 MAXSUN MS-iCraft B850 AIGA 在 Windows 重启后 `FAN5=MEM_FAN` 曲线失效的问题：持续读取 DIMM 温度并写入 NCT6796D `Virtual_TEMP`，不修改 BIOS、曲线、温度源或固件。

已确认链路：

```text
DIMM SPD → AMD FCH SMBus → DIMM 温度 → NCT Virtual_TEMP → BIOS Smart Fan → FAN5
```

Linux 版本已经完成实机闭环，是当前可交付的修复。Windows 工作只在能增加真实可用性时继续；不再为已证伪的 PnP 资源绑定模型继续堆代码。

## 2. 不变的硬件事实

- NCT 芯片：chip id `0xd802`，NCT6796D-S / NCT6799D 兼容系列。
- NCT SIO：index `0x295`、data `0x296`；标准 SIO `0x2e/0x2f`。
- `FAN5=MEM_FAN`：page `0x09`、reg `0x00` = source `0x0a`（`Virtual_TEMP`）。
- 写回目标：page `0x0c`、reg `0x36`；编码为整数摄氏度。
- FCH SMBus：实机诊断基址 `0xb00`，控制器 `PCI\VEN_1022&DEV_790B`。
- SPD 地址按 `0x53, 0x52, 0x51, 0x50` 轮询，命令 `0x31`，word read。
- 温度换算：`scaled=(raw << 3) >> 5`，`celsius=(scaled * 25) / 100`，有效范围 `0..120°C`。
- SMBus `HST_STS=0x02` 可表示成功；`0x04` 不能简单等同于失败类型，空槽与已安装 DIMM 的错误处理必须区分。
- NCT 页选择必须保留高 4 位：读取旧值后写 `(old & 0xf0) | page`。
- 实测：写 30°C → `pwm5=76`、约 1031 rpm；写 40°C → `pwm5=101`、约 1326 rpm。

## 3. 当前进展和结论（2026-09-05）

### 3.1 已完成

- Linux 服务完成读取 `spd5118` hwmon、最高有效 DIMM 温度选择、NCT 写回/读回校验、systemd 常驻和基本实机验证；发布物在 `release/linux/`。
- Windows 阶段 1 骨架、控制设备、只读 IOCTL、SMBus/NCT 纯逻辑检查和服务 `--once` 接口已完成。
- Windows 目标机已确认：Windows 11 10.0.26200 x64；Secure Boot False；2×16GB；AMD SMBus `DEV_790B`；`PNP0C02\700` 声明 `0xb00-0xb0f`，`PNP0C02\0` 声明 `0x290-0x29f`。
- `PNP0C02\700`、`\0` 均由 `machine.inf` 提供且 Enum 键无 `Service`，没有功能驱动 FDO。

### 3.2 已证伪、不得重做

实验 B/C 已证明：

- 通用 `PNP0C02` upper-filter 在运行期和开机栈构建均不挂载；驱动不会收到 `EvtDevicePrepareHardware`。
- `pnputil`、SetupAPI `DIF_INSTALLDEVICE`、`UpdateDriverForPlugAndPlayDevicesW` 均拒绝以第三方 function driver 替换目标节点。
- 因此“绑定 `PNP0C02` 并取得 translated resources”在本平台不能作为 Windows 实现前提。
- 非 PnP 驱动当前没有 translated resource list；`FEED_ONCE` 必须继续在任何端口访问前返回 `RAMFAN_FEED_HW_UNAVAILABLE`。
- 不再继续修改 INF、upper-filter、PNP0C02 function-driver 替换或用 PCI 资源授权 NCT 端口。
`LOG.md` 中早于 2026-09-05 的 PNP0C02 绑定方案仅是历史计划，不得恢复为当前实施计划或授权依据。

机器当前仍有测试遗留：testsigning 为 on，测试证书 `RAMFanTestSign` 在 Machine 存储中。若不立即进入 Windows 试验，先执行回滚脚本并重启。

## 4. 修复优先级与路线选择

按“能修复问题”排序：

1. **Linux 立即交付**：这是已验证的修复，不等待 Windows。
2. **Windows 受控可行性试验**：在明确批准后评估非 PnP KMDF + 固定端口访问模型。该模型不是当前授权政策下的正式实现，必须先完成身份、平台和签名边界设计，再允许一次性写回试验。
3. **Windows 正式交付**：只有存在可信签名、可接受的端口授权依据、可回滚安装方式和目标机闭环证据时才制作。
4. **SMM/BIOS 方案**：仅在 Windows 受控试验失败且机主另行批准后评估；本工作流不刷写 BIOS。

“Windows 必须绑定 PnP translated resources”不再是修复目标本身，而是已失败的授权方案。若机主不批准放宽模型，则 Windows 停止在阶段 1，项目以 Linux 版本交付。

## 5. Windows 受控可行性试验（必须先批准）

### 5.1 放宽模型的最小边界

候选实现为**非 PnP KMDF 控制设备 + 驱动内固定目标端口**，但必须同时满足：

- 只支持已确认的主板/芯片身份：通过只读 PCI `DEV_790B`、ACPI 设备/资源声明和 NCT chip id `0xd802` 进行拒绝式校验；任何一项不匹配即不工作。
- 端口白名单只允许 `0xb00` SMBus、`0x295/0x296` NCT SIO，以及仅用于 NCT 身份探针的标准 SIO `0x2e/0x2f`；不提供任意端口或任意寄存器 IOCTL。`0x2e/0x2f` 的解锁、chip-id 读取和锁定只允许在驱动内部执行，不能用于其他寄存器操作。
- 只允许写 NCT page `0x0c` / reg `0x36`；SMBus 事务、超时清理、页恢复和写后读回必须在驱动内完成并加锁。
- 明确记录：ACPI 资源声明是诊断/平台绑定证据，不等于独占；NCT/SMBus 可能被固件、ACPI/WMI 或监控软件并发访问。
- 仅允许测试机、管理员/SYSTEM、明确签名的驱动和可逆安装；不得关闭签名策略作为发布方案，不得引入 WinRing0/InpOut32。

在实现前，必须由机主确认这是一项**有风险的受控访问模型**，并同步修订 `AGENTS.md` 的资源授权条款；未经确认，驱动继续阻断写回。

### 5.1a 授权表与不可推导事项

必须分开记录以下四件事：

- **身份依据**：主板、PCI `DEV_790B`、ACPI 设备/资源声明、NCT chip id；用于拒绝不匹配硬件。
- **访问依据**：当前没有已确认的 Windows 资源持有或独占授权。身份匹配和 ACPI 声明不能推出访问授权；只有机主批准的目标机受控实验例外，才可承担共享端口风险，且该例外不构成正式交付依据。若项目不接受此例外，Windows 路线立即停止。
- **实验批准**：机主是否明确批准在这台目标机承担共享端口风险；这只允许受控实验，不自动变成发布授权。
- **发布批准**：是否具备面向其他机器交付的签名、支持范围和端口访问依据。当前候选模型默认不具备。

本候选模型没有可宣称的 Windows 独占资源授权。ACPI/PNP0C02 声明、PCI BAR 和历史 Linux 基址只能作为拒绝式平台校验或诊断证据；它们不能单独授权非 PnP 驱动访问，也不能证明端口独占。进入真实端口读写前，必须在决策记录中明确“受控实验例外”而非伪称为 OS 资源授权；正式发布仍必须有独立、可验证的访问依据。若不能接受该风险，回到 Linux 交付。

### 5.2 试验顺序

进入第 1 步前必须同时满足：机主对本机受控试验明确批准；`AGENTS.md` 已同步记录新授权边界；驱动可加载/卸载、控制设备 ACL 仅允许 SYSTEM/管理员、IOCTL 默认阻断、停止服务后无活动请求和残留设备对象；安装失败可恢复原状态。每次从只读阶段进入写回阶段前，须有独立子代理审查和实机证据记录。

1. **先做只读身份门禁**：实现并测试平台匹配、NCT chip id、SMBus 控制器识别；NCT 身份探针可按 §5.1 的严格白名单短暂操作 `0x2e/0x2f`，但仍不访问 SMBus 事务寄存器、不写 NCT 目标寄存器。
2. **读取 DIMM 数据的受控 SMBus 试验**：这里的“只读”仅指对 DIMM 数据读取；SMBus word-read 必然会向控制器状态、地址、命令和启动寄存器写入事务，因此必须单独取得受控实验批准。停止 HWiNFO、OpenHardwareMonitor、AIDA、Linux 环境及其他端口工具后，探测 `0x53..0x50`，记录空槽 NACK、已安装 DIMM、状态位、超时和重复读取稳定性。任何已安装 DIMM 的异常使整轮失败。
3. **单次写回**：恢复 `FEED_ONCE`，一次完成全部 DIMM 读取、最高温度选择、NCT 写入和读回校验。先记录 NCT 原页/原值和 `pwm5/fan5` 基线；写入只允许 `0..120°C`。
4. **动态闭环**：验证 30°C/40°C 已知响应、DIMM 温升下的单调变化、服务重启和睡眠恢复。不得以一次转速读数代替温度链路证据。
5. **失败试验**：覆盖空槽、已装 DIMM 失败、全部失败、SMBus BUSY 超时、NCT 读回不一致、外部并发；失败时不写 0°C、不写猜测值，保留旧值并记录热安全风险。
6. **回滚**：停止服务、卸载驱动、关闭 testsigning、删除测试证书并重启；确认 BIOS 曲线/温度源未改变。回滚验收还必须确认 `RAMFanPnP`/控制设备/服务/驱动文件/DriverStore 无残留，`UpperFilters`、`Service`、`ConfigFlags` 恢复原状，重启后原设备栈恢复，且没有仍驻留的旧驱动或活动请求。NCT 最后一笔有效 `Virtual_TEMP` 不主动清除，但须记录该行为；回滚失败时禁止继续端口访问。
任一身份误判、端口访问不稳定、超时无法恢复、读回不一致、外部并发不可控或回滚失败，立即停止写回并回到 Linux 交付，不扩展功能。

## 6. Windows 常驻服务与正式交付门槛


当前 Windows 仍停留在“候选访问模型决策”阶段，尚未进入阶段 2 写回；以下常驻服务和正式交付条件是未来门槛，不是当前执行计划。
只有单次写回闭环通过后，才实现/启用服务的 0.5 秒循环。开发阶段使用 demand start；服务重启、睡眠恢复和系统重启均验证通过后才考虑自动启动。

正式交付还必须满足：

- x64 Release 构建、静态检查、纯逻辑自检、安装/卸载/回滚验证通过。
- 测试机可使用测试证书和 testsigning，但只能标记为开发测试版；正式 Windows 发布必须满足目标 Windows/Secure Boot 策略可接受的 Microsoft 可信签名要求，例如 Attestation 或 WHQL。
- 必须验证 `.sys`、catalog、页哈希和安装包签名；当前目标机 Secure Boot 关闭只影响开发实验，不降低正式发布门槛。不得通过关闭 Secure Boot、开启 testsigning 或绕过 CI 作为交付方案。
- 日志记录连接失败、有效温度、写入/读回、连续失败和恢复，但不每 0.5 秒刷屏。
- 明确说明服务停止不清除最后写入值；连续失败造成旧值过期是热安全风险。
- 由独立子代理审查寄存器、SMBus 状态、温度换算、IOCTL 边界、并发、失败策略、签名和回滚后，才允许提交/打包。

## 7. Linux 交付路线

Windows 决策期间不改造 Linux 链路。发布/维护只做必要修复：

- 读取 `spd5118` hwmon，读取不完整时跳过写入并保留上次完整值。
- 写 NCT page `0x0c` / reg `0x36`，写后读回校验。
- 周期默认 0.5 秒，温度有效范围 `0..120°C`。
- 修改后运行 `cargo fmt`、`cargo check`、`cargo test`、`cargo clippy`。

## 8. 文档、审查与记录规则

- 每个决策先写入根目录 `LOG.md`：选择、依据、风险、是否批准。
- Windows `patch/` 的任何非文档代码修改，先查现有调用链；完成后必须独立子代理审查，再提交。
- 所有实机试验记录 OS、BIOS、内存、基线、命令、结果、签名状态、回滚状态和未决风险。
- 不刷 BIOS、不修改 BIOS 变量、不修改 `page 0x09` 曲线/模式/温度源。
- 不同时运行多个会写 `0x295/0x296` 或访问 SMBus 的工具。

## 9. 当前下一步

1. 机主决定是否批准第 5 节的受控非 PnP 访问模型。
2. 未批准：执行 `patch/windows/experiment-b-rollback.ps1 -RemoveCert`，重启关闭 testsigning，Windows 停止，维护 Linux 发布。
未批准 Windows 时的 Linux 收尾还必须确认：Windows 测试服务、驱动、DriverStore、UpperFilters 和测试证书均已清理；testsigning 已关闭并在重启后生效；Linux release 包、systemd unit 和使用说明一致；不得把 Windows 测试版描述为正式修复方案。
3. 已批准：先修订 `AGENTS.md`/本文件的授权边界，再只实现第 5.2 的身份门禁和只读 SMBus 阶段；不得直接恢复写回。
4. 任何 Windows 试验结果写回 `LOG.md`，通过独立审查后再进入下一阶段。
