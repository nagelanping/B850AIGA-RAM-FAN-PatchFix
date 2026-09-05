# AGENTS.md

## 项目简介

本项目修复铭瑄 MAXSUN MS-iCraft B850 AIGA 主板的“内存风扇自定义曲线重启后失效”问题。

根因已确认：固件把 `FAN5=MEM_FAN` 的温度源配置为 NCT6796D 的 `Virtual_TEMP`（source `0x0a`）。该通道没有硬件数据，必须由软件持续喂值；固件只在 BIOS 打开内存风扇曲线页面时喂值，重启后停止，风扇因此回到约 941 rpm。

**已定案的修复方向**：软件补丁优先。Linux 和 Windows 执行相同的核心逻辑：读取 DIMM 温度，写入 NCT `Virtual_TEMP`。SMM 固件补丁只作为最后备选，不得未经明确批准刷写 BIOS。

## 关键已确认数据（勿重新调查）

- `Virtual_TEMP` 值寄存器：NCT page `0x0c`、reg `0x36`；编码为 `°C × 1`，例如 `0x1e` = 30°C。
- 实测响应：写 30°C → `pwm5=76`、约 1031 rpm；写 40°C → `pwm5=101`、约 1326 rpm。
- DIMM 温度来源：AMD FCH/南桥 SMBus，历史实测基址 `0xb00`。
- SPD 访问：7-bit 地址优先轮询 `0x53, 0x52, 0x51, 0x50`，命令 `0x31`，读取 2 字节 word。
- 温度换算：`scaled = (raw << 3) >> 5`；`celsius = (scaled * 25) / 100`。
- SMBus 状态：`HST_STS bit1 (0x02)` 可在成功事务中置位；`0x04` 是无设备/CRC 样式错误。不能把 `0x02` 单独判为失败。
- NCT SIO 端口：index `0x295`、data `0x296`。页选择必须保留高 4 位：

  ```text
  outb(0x4e, 0x295)
  v = inb(0x296)
  outb((v & 0xf0) | page, 0x296)
  ```
- 芯片：实测 chip id `0xd802`，NCT6796D-S / NCT6799D 兼容系列。
- 内存风扇：`FAN5=MEM_FAN`，page/bank `0x09`；其 reg `0x00` 为温度源选择，值为 `0x0a`。
- NCT 自身 `SMBUSMASTER` 读取 DIMM 的路径已排除；NCT 硬件温度源没有可用 DDR 温度。

完整历史证据、固件地址和实验记录在 `archive/0.1/linux/LOG.md`；当前进度和 Windows 结果记录在根目录 `LOG.md`。

## 目录结构

- `LOG.md` — 当前项目进度、Windows 结果、待办和新发现。
- `WORKFLOW.md` — 当前 Windows 开发工作流、阶段门槛和验收标准。
- `archive/0.1/linux/` — Linux 0.1 的归档 `LOG.md`、`WORKFLOW.md` 和历史工作记录，不作为当前 Windows 状态文件。
- `patch/linux/` — 已完成的 Linux Rust 服务、systemd unit 和开发文档。
- `patch/windows/` — Windows KMDF 驱动和 Windows Service，后续开发目录。
- `release/linux/` — Linux 发布包。
- `ref/` — 只读逆向参考：
  - `ref/bios/` — BIOS 镜像、UEFIExtract 解包、report、GUID 列表；
  - `ref/disasm/` — M351、SkSmartFanProtocol、SkSmartFanCtrlPei、Setup 反汇编；
  - `ref/mods/` — 固件模块二进制；
  - `ref/kernel/nct6775-core.c` — NCT 驱动源码和温度源参考；
  - `ref/scripts/` — 硬件探测与实验脚本；
  - `ref/misc/` — TE 到 flat 映像等逆向辅助材料。
- `tmp/` — 临时分析物，已 gitignore，不存放唯一有价值的证据。

## Linux 维护边界

Linux 版本已基本完成实机闭环和 systemd 常驻验证；系统重启恢复和有效动态温升仍以归档记录为准。后续只做维护、修复和发布，不因 Windows 开发重新设计其链路。

- 读取后端使用 `spd5118` hwmon sysfs，由 Linux 内核负责南桥 SMBus 访问，避免用户态直接抢占 SMBus。
- 写入后端使用 `/dev/port`，只写 NCT page `0x0c` / reg `0x36`，写后读回校验。
- 默认周期为 0.5 秒；读取全部有效 DIMM，取最高温度；本轮读取不完整时跳过写入并保留上一次完整值。
- 温度有效范围为 `0..120°C`；异常值、负值和读取不完整均不得写入 NCT。
- 运行时依赖为 Rust 标准库；修改后至少运行 `cargo fmt`、`cargo check`、`cargo test`、`cargo clippy`。
- Linux 用户态多步 SIO 访问无法与 `nct6775` 完全原子协调；已有短时并发观察无毛刺，但该理论竞态仍须保留为已知风险。
- 不修改 BIOS、风扇曲线、温度源选择或 `nct6775` 内核驱动。

## Windows 开发边界

Windows 版本优先采用 **KMDF 内核驱动 + Windows Service**，不把 WinRing0、InpOut32 等第三方驱动作为正式依赖。

### 授权边界（2026-09-05 机主批准受控实验例外）

目标机 `PNP0C02\700`/`\0` 无功能驱动 FDO，PnP 绑定路径（upper-filter 与 function-driver 替换）已证伪。Windows 仅以**非 PnP KMDF 控制设备 + 驱动内固定目标端口**的受控模型继续；该模型只适用于机主明确批准的目标机受控实验，**不是正式发布依据**。四件事分开记录：

- **身份依据**：主板型号、PCI `DEV_790B`、ACPI `PNP0C02` 设备/资源声明、NCT chip id `0xd802`。只用于拒绝式校验：任何一项不匹配即不工作。
- **访问依据**：当前没有已确认的 Windows 资源持有或独占授权。身份匹配和 ACPI 声明不能推出访问授权；只读 PCI/ACPI 证据不得作为非 PnP 端口访问的合法化依据。
- **实验批准**：机主于 2026-09-05 批准在本目标机承担共享端口风险的受控实验例外，允许驱动内部对固定白名单端口做**只读身份探针**；批准只覆盖受控实验，不自动变成发布授权。
- **发布批准**：候选模型默认不具备。正式 Windows 发布仍须满足可信签名（Attestation/WHQL 等）与独立、可验证的访问依据。

受控模型硬约束：

- 端口白名单固定为 `0xb00`（SMBus）、`0x295/0x296`（NCT 自定义 SIO）、`0x2e/0x2f`（标准 SIO，仅用于 NCT chip-id 身份探针，解锁-读-锁定一次完成）；不提供任意端口或任意寄存器 IOCTL。
- 禁止以关闭 Secure Boot/testsigning 或自声明端口作为交付方案；测试证书与 testsigning 只用于本机开发测试，交付前须回滚关闭。
- ACPI 资源声明只作诊断/平台绑定证据，不等于端口独占；NCT/SMBus 可能被固件、ACPI/WMI 或监控软件并发访问。

### 驱动职责
- 驱动负责端口 I/O、SMBus 完整事务、状态位判断、温度换算、NCT 页选择、目标寄存器写回和读回校验。
- 服务只调用最小的 `IOCTL_RAM_FAN_FEED_ONCE`，不接触裸端口，不接收任意寄存器地址。
- 一次 IOCTL 完成“读取已安装 DIMM → 校验 → 取最高温 → 写入并读回校验”，驱动内部锁住完整序列。
- 端口访问只允许驱动内的受控身份探针与（后续批准的）SMBus/NCT 事务；先按目标机资源证据完成拒绝式身份匹配（主板/`DEV_790B`/NCT chip id `0xd802`），不匹配则 `FEED_ONCE` 与读写一律拒绝。
- 启动时检查目标 FCH SMBus 控制器和 NCT chip id；硬件不匹配则拒绝工作。
- SMBus 超时必须有有限的状态清理/中止路径；不对共享控制器执行未经验证的强制复位。
- NCT 写入前保存当前页，成功或失败时尽力恢复；只允许写 page `0x0c` / reg `0x36`。
- 首轮先建立“已安装 DIMM”与 SPD 地址的映射：空槽 NACK 可忽略；已安装 DIMM 的超时、总线错误或异常状态使本轮整体失败，不能用剩余低温样本降速。

### 服务职责

- 服务负责 SCM 生命周期、0.5 秒调度、有限退避、`--once`、日志和错误重试。
- 开发测试阶段使用 `SERVICE_DEMAND_START`；验收通过后才切换为自动启动。
- `--once` 返回：成功 `0`，硬件或 I/O 失败 `1`，参数错误 `2`。
- 服务停止不清除 NCT 最后写入值；读取失败时不写 0°C、不写猜测值。
- 连续失败导致旧值过期是已知热安全风险，不能把“保持旧值”描述为完整 fail-safe；未经验证不得擅自写安全温度。
- 驱动内部锁不能解决 HWiNFO、其他监控软件、ACPI/WMI 或固件访问造成的外部并发；目标机测试时停止其他会写 NCT/SMBus 的工具。

### Windows 阶段门槛

1. 记录 Windows 版本、Secure Boot、驱动签名策略、内存条数量、SMBus 控制器和资源信息（已完成）。
2. 只读身份门禁：驱动加载/卸载、设备句柄、拒绝式身份匹配与只读身份探针；此阶段禁止 SMBus 事务寄存器访问和 NCT 写。
3. 受控 SMBus 试验：读 DIMM 数据（涉及事务寄存器写入，需单独批准），验证空槽 NACK 与已安装 DIMM 失败语义、状态位、超时与重复读取稳定性。
4. 单次写回：恢复 `FEED_ONCE`，一次完成全读取→校验→最高温→NCT 写回并读回校验。
5. 常驻服务：0.5 秒调度，验证服务重启、睡眠恢复和系统重启后自动恢复。
6. 完成静态检查、x64 Release 构建、安装/卸载/回滚、动态温升和日志验证。
7. Secure Boot 下正式交付必须有 Microsoft Attestation、WHQL 等可信签名；测试签名版只能作为开发测试版，不得绕过签名策略发布。

## 实机操作约束

- 当前 Agent 会话无 Windows 实机终端；需要管理员权限的 Windows 驱动安装、端口访问和硬件测试必须由机主执行。提供命令和检查项，不假设命令已执行。
- Linux 实机需要 root 的 `/dev/port` 操作也必须由机主执行；每次重启后通常先执行 `sudo modprobe nct6775`，出现对应 hwmon 后再读取风扇数据。
- 写 SIO 寄存器可逆，修改前记录原值；重启通常可由 BIOS 恢复。禁止未经明确批准刷 BIOS。
- 不同时运行多个会写 `0x295/0x296` 的补丁或硬件监控工具。
- 任何实机测试都要记录操作系统、BIOS、硬件、基线、命令、结果和未决风险。

## 已知陷阱

- 当前参考路径使用 `ref/`，Linux 历史文件使用 `archive/0.1/linux/`；不要恢复旧 `tmp/` 路径。
- `temp_sel` 是 Linux hwmon 通道号索引，不是 NCT source 编码。
- `SkSmartFanSetupData` 不存在于 efivarfs，不能依赖 OS 侧 UEFI 变量持久化。
- NCT source `0x0a` 是 `Virtual_TEMP`，不要把它当作可直接读取的 DDR 硬件温度。
- Windows 中 SPD 7-bit 地址和 SMBus 写入地址字节不要混淆：`0x53` 对应读地址字节 `0xa7`；固件记录的 `0xa6` 是另一种写格式表示。
- Ghidra 12.1.2 与 JDK26 不兼容；固件分析使用已有 objdump/flat-image 结果，不为 Windows 开发重复建立 Ghidra 流程。
- 系统无 `xxd` 时使用 `od`。
- Windows 非 PnP 身份门禁只允许两类只读硬件接触：PCI 配置空间读取（`DEV_790B` 存在性）与标准 SIO `0x2e/0x2f` 解锁→读 chip id→锁定；不得在身份门禁路径访问 SMBus 事务寄存器（`0xb00` 偏移 `0x00-0x06`）、NCT 自定义端口 `0x295/0x296` 或写 page `0x0c`。`HwMatched` 只要求控制器存在 + chip id==0xd802，不要求 SMBus BAR 等于 0xb00。
- 内核服务名沿用历史名 `RAMFanPnP`（2026-09-05 前的实验遗留名），实际是**非 PnP** 内核服务；不要在文档中把它描述为 PnP filter，改名须同步 prep/rollback/WINDOWS.md/回滚验收。
- `HalGetBusDataByOffset` 在 x64 上对跨 bus 的 PCI 配置访问支持可能受限（实际可能只覆盖 bus 0）；身份门禁扫描 bus 0-15 属尽力而为，实机运行时须记录扫描覆盖，非目标机/找不到控制器的失败方向是拒绝（安全方向）。
- Windows 实机已验证：目标 `PNP0C02\700`/`\0` 由 machine.inf 提供且 Enum 键无 Service（无功能驱动 FDO），upper filter 无法附加（运行期与开机栈构建均不生效），function-driver 替换被 pnputil/SetupDi/UpdateDriver 拒绝；因此“绑定 PnP 设备持有 translated resources”的 Windows 访问模型在本平台不可行，不得以自声明端口或绕过签名方式交付（WORKFLOW §7，2026-09-05）。已删除的 `resource_model.c` 角色分类逻辑属于该已证伪路径，不得恢复。

## 工作规则

1. 开始任务前阅读根目录 `LOG.md`、`WORKFLOW.md`；涉及 Linux 历史证据时再读 `archive/0.1/linux/`，避免重复调查。
2. 先查找现有实现和调用方，再新增代码；保持双系统核心数据链路一致，但使用各自系统的合法硬件访问接口。
3. 不增加未经需要的 GUI、配置协议、可调周期、多后端或框架；先完成最小可验证闭环。
4. 非平凡硬件逻辑至少留下一个可运行检查：单元测试、模拟测试或最小 `--once` 自检。
5. 完成后更新根目录 `LOG.md` 的状态、证据、待办和风险；不要用删减历史的方式更新归档文件。
6. 若新增可 grep 的硬件常量、访问陷阱或开发约束，同步维护本文件。
7. 提交前检查 diff，确认没有误写曲线、温度源、BIOS 或无关寄存器。

## 审查与提交

**每项大任务（尤其是 `patch/` 下的修改）完成后，必须交由子代理独立审查；审查通过并确认无误后才允许提交。**

审查至少覆盖：寄存器和端口、SMBus 状态位、温度换算、Linux/Windows 行为、Windows IOCTL 边界、资源与签名限制、并发、失败和停止策略、与 `LOG.md` 已确认事实的一致性，以及是否引入破坏性副作用。提交使用声明式 commit message。
