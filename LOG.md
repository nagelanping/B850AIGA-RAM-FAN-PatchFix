# LOG — MS-iCraft B850 AIGA 内存风扇曲线不生效

## 当前状态（置顶）

- **Linux 补丁初版已实现（未实机测试）**：`patch/linux/` 完成 Rust 零依赖实现（`port.rs`/`smbus.rs`/`nct.rs`/`main.rs` + systemd unit + README），`cargo fmt/check/test/clippy` 全通过，含 `--once` 阶段 B 验证入口。等待机主实机测试（命令见 `patch/linux/README.md`）。
- **根因已确认**：内存风扇温度源被固件配置为 NCT6796D 的 `Virtual_TEMP`（src `0x0a`）。该通道没有硬件数据，必须由软件持续写入；固件只在 BIOS 打开内存风扇曲线页时喂值，重启后喂值停止，因此风扇回到低档（约 941 rpm）。
- **关键链路已完整逆向**：南桥 SMBus `0xb00` 可读 DIMM 温度；温度写入 NCT 页 `0x0c`、reg `0x36` 后，`FAN5=MEM_FAN` 按曲线响应。
- **前置实验已完成**：Virtual_TEMP 值寄存器定位成功；NCT 自身 `SMBUSMASTER` 路径排除。
- **修复路线已定案**：软件补丁优先，Linux systemd 服务 + Windows 驱动/服务；SMM 固件补丁仅作备选，不先刷 BIOS。
- **当前待办**：先实现并实测 Linux 服务，再实现 Windows 方案；只有 OS 方案不可行时才考虑 SMM。

## 最后更新

**2025-09-04**：完成 M351/SkSmartFanProtocol 喂值链路逆向；完成 Virtual_TEMP 值寄存器实验和 SMBUSMASTER 排除实验；确定双系统 OS 层软件补丁路线。

## 已确认关键数据（后续工作直接使用）

| 项目                  | 已确认值                                                                           |
| --------------------- | ---------------------------------------------------------------------------------- |
| 芯片                  | Nuvoton NCT6796D/NCT6799D 兼容系列；实测 chip id`0xd802`，具体型号以驱动识别为准 |
| NCT 固件访问端口      | `0x295/0x296`                                                                    |
| NCT 标准 SIO 端口     | `0x2e/0x2f`；Linux 绑定基址 `0x290`                                            |
| 内存风扇              | `FAN5=MEM_FAN`，bank code `0x09`                                               |
| 内存风扇温度源        | bank 内 reg`0x00` = `0x0a`（Virtual_TEMP）                                     |
| Virtual_TEMP 值寄存器 | NCT 页`0x0c`、reg `0x36`                                                       |
| 温度编码              | `°C × 1`，例如 `0x1e` = 30°C                                                |
| DIMM 温度来源         | 南桥 SMBus 基址`0xb00`                                                           |
| SMBus 访问            | 从地址`0x53`（读写地址 `0xa6/0xa7`），命令 `0x31`，读 2 字节                 |
| 温度换算              | `(raw << 3 >> 5) * 25 / 100`，实测 38.0°C                                       |
| 写入目标              | 选择 NCT 页`0x0c`，写 reg `0x36`                                               |
| 建议周期              | 约 2 秒                                                                            |

NCT 页选择序列：

```text
outb(0x4e, 0x295)
v = inb(0x296)
outb((v & 0xf0) | code, 0x296)
```

## 修复计划

### 首选：OS 层软件补丁

两套系统执行同一逻辑：每约 2 秒从南桥 SMBus 读取 DIMM 温度，换算为摄氏度整数，再写入 NCT 页 `0x0c` reg `0x36`。

1. **Linux**：systemd 服务；使用 `/dev/port`（root）访问 `0xb00` SMBus 和 `0x295/0x296` NCT 端口。可用 Rust/C/Python，优先复用 `ref/scripts/` 中已验证逻辑。
2. **Windows**：内核驱动 + 服务；KMDF/WDM，或评估现成签名驱动（WinRing0/InpOut32）。可能涉及关闭 Secure Boot 或启用测试签名。
3. 每个系统均需处理 SMBus 读失败、NCT 写失败和服务退出；停止服务不会清除最后一次写入的 Virtual_TEMP 值，通常会保持到 NCT 复位/重启，因此 stale/fail-safe 策略必须单独定义。

优点：不刷写固件、核心链路已实机验证、易调试。缺点：需要维护 Linux 和 Windows 两套实现。

### 备选：SMM 固件补丁

在 SMM 中注册周期 SMI，每次执行“读 SMBus DIMM 温度 → 写 Virtual_TEMP”。该方案不依赖 OS，但需要构造 SMM 驱动、处理 SMRAM/封包并刷写 BIOS；SMM 崩溃可能导致死机，刷写存在变砖风险。只有软件补丁不可行时再考虑。

### 不满足要求的方案

- PEI/DXE 启动时只喂一次：OS 下温度冻结，不满足持续更新。
- DXE 定时器：`ExitBootServices` 后停止，不满足 OS 下持续运行。
- NCT `SMBUSMASTER` 硬件自读：已排除，NCT SMBus 未物理连接 DIMM 总线。
- 将数据源改为 CPU 温度：已验证可用，是零成本临时方案，但语义变为随 CPU 温度变化。

## 实机操作约束

- 本机为 Arch Linux 实机；Agent 无终端，需 root 的 `/dev/port` 操作必须由机主执行。
- 每次重启后需先执行 `sudo modprobe nct6775`；出现 `hwmon9` 后才有 NCT 风扇/PWM/温度读数。
- 写 SIO 寄存器可逆，重启后 BIOS 会恢复；修改前记录原值。
- 不要把 sysfs `temp_sel` 当作 SRC 编码：它是通道号索引。
- `SkSmartFanSetupData` 不存在于 efivarfs，不能依赖 OS 侧 UEFI 变量持久化。
- 禁止未经明确批准刷写 BIOS；刷写前必须备份并确认签名校验情况。

## 后续工作常用命令

在仓库根目录执行：

```bash
sudo modprobe nct6775
sudo python3 ref/scripts/sio_probe.py
sudo python3 ref/scripts/sio_probe2.py
sudo python3 ref/scripts/exp1_virtemp_probe.py
sudo python3 ref/scripts/exp2_smbus_probe.py
```

- hwmon：`/sys/class/hwmon/hwmon9/{temp*_input,fan*_input,pwm*}`
- 原始探测输出：`ref/scripts/sio_dump.txt`
- root 访问方式：Python 使用 `os.pread/pwrite` 读写 `/dev/port`

## 目录和参考资料

- `patch/linux/`、`patch/windows/`：补丁实现目录
- `ref/bios/`：BIOS 镜像、UEFIExtract 解包结果、report、GUID 列表
- `ref/disasm/`：M351、SkSmartFanProtocol、SkSmartFanCtrlPei、Setup 反汇编
- `ref/mods/`：`M351.bin`、`SkSmartFanCtrlPei.bin`、`SkSmartFanProtocol.bin`、`Setup.bin` 等
- `ref/kernel/nct6775-core.c`：Linux NCT 驱动源码，寄存器表参考
- `ref/scripts/`：探测和实验脚本
- `ref/misc/`：TE 到 flat 映像等逆向辅助文件
- `LOG.md`：本档案；所有进度、新发现和待办写在这里

---

# 详细逆向档案

## 1. 背景与现象

- 主板：铭瑄 MAXSUN `MS-iCraft B850 AIGA`（16？），BIOS `E1.6D`（`MSiCraftB850AIGA_E1.6D_011526.ROM`，MD5 `e2ab3aa79830d1af5ba5fe2e6082719e`，32 MiB AMI Aptio）。所有 BIOS 版本均存在此问题。
- SMBIOS 风扇头字符串：`FAN1:SYS_FAN1 FAN2:CPU_FAN FAN3:SYS_FAN2 FAN4:SYS_FAN3 FAN5:MEM_FAN FAN7:PUMP_FAN1`。
- BIOS 风扇设置页：CPU 风扇、系统风扇 1/2/3、内存风扇、水泵；包含“数据源/温度源”下拉项。
- 默认状态：仅水泵为全速，其余为默认曲线。
- 自定义内存风扇曲线改完 F10 后自动重启即失效；进 Linux 或 BIOS 均相同。
- 只有在 BIOS 会话内点开内存风扇曲线页后曲线才生效，并持续到下次重启。
- 失效时内存风扇约 941 rpm，内存温度 35→45°C 时转速几乎不变。
- 全速模式可持久化；其他风扇的自定义曲线重启后正常。
- 数据源改为 CPU 温度后，内存风扇随 CPU 温度正常响应。
- 内存插在 2、4 通道；Windows HWinfo 能正常显示独立内存温度。

## 2. 映像结构与模块

- 解包工具：`UEFIExtract`（`/usr/bin/uefiextract`）；产物在 `ref/bios/bios.rom.dump/`，全览文件为 `ref/bios/bios.rom.report.txt` 和 `ref/bios/bios.rom.guids.csv`。
- 主板实为 MAXSUN，代码风格类似 MSI 系 ODM，模块前缀为 `Sk*`。
- `SkSmartFanCtrlPei`（GUID `54615123-39EE-49EA-8FFD-9F2E358A400B`）：PEI 启动风扇编程，x86 TE，entry RVA `0x399`，ImageBase 约 `0x9c02be4`。
- `SkSmartFanProtocol`：DXE 协议，采样 SIO、390B（6×5×13）默认表及 18 个协议函数。
- `M351`（GUID `A2DF5376-C2ED-49C0-90FF-8B173B0FD066`）：647 KiB DXE 风扇主驱动，处理日志、`SkSmartFanSetupData`/`SkWarningInfo` 和 RPM 显示；`SetVariable` 回调应用风扇配置。
- `Setup`（GUID `899407D7-…`）：493 KiB DXE，AMI Setup，IFR 表单，VarStore `0x22`。
- `ProjectSmi/ProjectPei/ProjectDxe`：MAXSUN 自定义模块；`ProjectSmi` 仅注册 SW SMI 3/4/5 空 stub。

## 3. 风扇配置变量与 PEI 写入

变量：`SkSmartFanSetupData`，GUID `CE8621F0-3B53-48E4-A014-14A739F54EB9`，长度 112（`0x70`）。

- Setup IFR VarStore id `0x22`，6 个 FORM：`0x0b/0x16/0x21/0x2c/0x37/0x42`。
- 变量由 6 块、每块 14 字节组成，块偏移 `0/14/28/42/56/70`。
- 块内 `[0]` 为模式（0 Standard、1 Silent、2 Performance、3 FullSpeed、4 Defined/自定义）；`[1..3]` 为选择器；`[4..12]` 为 9 个曲线字节；`[13]` 为第 13 字节。
- efivarfs 中有 75 个 UEFI 变量，但没有 `SkSmartFanSetupData` 或风扇变量；该变量只供固件内部 PEI/DXE 使用。

PEI 每个通道处理 13 字节结构：

- 温度源：bank 内 reg `0x00`。
- 模式/固定值：`0x03/0x04`、`0x45=2`。
- 曲线温度点：`0x21/0x22/0x23/0x24/0x35`。
- 曲线 PWM：`0x27/0x28/0x29/0x2a`，值按 `×255/100` 写入。
- 源为 1/2/3/4 时，按 struct[0] 写入 `0x0a/0x0b/0x10/0x1f/0x20` 等源寄存器。

| 变量块 | UI                 | bank               | 温度源                            |
| ------ | ------------------ | ------------------ | --------------------------------- |
| 1      | CPU 风扇           | `0x02`           | `0x10` = PECI Agent 0           |
| 2      | 系统风扇 1         | `0x01`           | `0x01` = SYSTIN                 |
| 3      | 系统风扇 2         | `0x03`           | `0x01`                          |
| 4      | 系统风扇 3         | `0x08`           | `0x01`                          |
| 5      | **内存风扇** | **`0x09`** | **`0x0a` = Virtual_TEMP** |
| 6      | 水泵               | `0x0b`           | `0x10`                          |

bank 对应 NCT 寄存器页高字节；内存风扇 bank `0x09` 对应 `FAN5`。温度源是 bank 内 `0x00`，曲线温度点是 `0x21–0x24/0x35`，曲线 PWM 是 `0x27–0x2a`。

## 4. NCT 温度源编号

依据 `ref/kernel/nct6775-core.c`：

```text
src 1=SYSTIN  2=CPUTIN  3..7=AUXTIN0-4
src 8=SMBUSMASTER0  9=SMBUSMASTER1
src 10/11/31=Virtual_TEMP  16=PECI Agent0  17=PECI Agent1
src 18-21=PCH_*  22-25=Agent0/1 Dimm0/1  26/27=BYTE_TEMP0/1
```

Virtual_TEMP 的内核掩码为 `NCT6796_VIRT_TEMP_MASK=0x80000c00`。NCT 硬件 18 路温度源中没有可用 DDR 温度；DDR 温度只在 AMD SoC 侧 `spd5118`（实测 hwmon5=33°C、hwmon7=37°C）。

## 5. 固件喂值链路

### 温度读取

- M351 `0x223e4`：SMBus HST 底层读写；`0x22498/0x224f0/0x22554/0x225b8/0x226c0/0x227c8…`：各 DIMM 槽位封装。
- SkSmartFanProtocol `0x1614/0x183c`：同款 SMBus 读取。
- SMBus：基址 `0xb00`；状态 `0xb00`、控制 `0xb02`、命令 `0xb03`、从地址 `0xb04`、数据 `0xb05–0xb06`。
- 从设备 `0xa6/0xa7`（SPD 地址 `0x53` 的读写位），命令 `0x31`，读取 2 字节。
- 温度换算为 `(raw << 3 >> 5) × 25 / 100`。
- 失败时地址按 `0xa6 → 0xa4 → 0xa2 → 0xa0` 递减轮询，覆盖 3 个槽位。

### NCT 写回

- SkSmartFanProtocol `0x183c`：读 SMBus、换算得到 `cl`，再通过 `0x295/0x296` 写 NCT reg `0x30/0x2e/0x32/0x34/0x36/0x38/0x3b`。
- `0x19ec`（`0x1834` 的 jmp 入口）执行同一模式，写 `0x31/0x2f/0x33/0x35/0x37`。
- M351 `0xf0000–0xf0bxx` 是内存温度采集/打印主逻辑，循环调用 `0x227c8/0x2288c/0x22980/0x22a44/0x22b38/0x22c2c` 等逐槽读取。
- 每次写寄存器前都会重复选择 NCT 页 `0x0c` 的端口序列。

### 触发和失效机制

- M351 `SetVariable` 回调位于 `0x13580–0x136ed`，引用 `SkSmartFanSetupData` name `0x1ab290`、GUID `0x169dc0`。
- 打开曲线页后，Setup 驱动轮询/显示内存温度，触发上述读温度并写回 NCT 的链路。
- 值寄存器在掉电/复位前保持，所以打开页面后可持续生效到下一次重启。
- 重启时 SIO 芯片复位、PEI 重写配置，Virtual_TEMP 值被清除；ExitBootServices 后 DXE 喂值代码消亡，OS 下无人继续写入。
- CPU/PCH/系统温度是 NCT 硬件自主采样，故 OS 下正常；Virtual_TEMP 只读不写，`nct6775` 没有喂值代码。

## 6. 实验记录

### 6.1 efivarfs

`/sys/firmware/efi/efivars/` 共 75 个变量，无 `SkSmartFanSetupData`、无风扇变量；`SkWarningInfo` 存在，`HiiDB` 为 12 字节。

### 6.2 SIO 寄存器

`ref/scripts/sio_probe.py` 使用 `0x295/0x296` 序列读取。完整输出在 `ref/scripts/sio_dump.txt`，深度扫描可用 `sio_probe2.py`。

- 曲线边界寄存器与用户设置逐字节一致；内存块 `0x27–0x2a = 33 4c 7f b2`，对应 20/30/50/70%。
- 内存曲线温度点 `0x21–0x24 = 00 1e 32 3c`、`0x35=0x50`，对应 0/30/50/60/80°C。
- bank 内 `0x00` 的温度源与 §3 一致。
- 自定义口 `0x20/0x21` 读到 `0xff`，芯片 ID 需从标准端口读取。

### 6.3 nct6775 绑定

```text
sudo modprobe nct6775
dmesg: nct6775: Found NCT6796D-S/NCT6799D-R or compatible chip at 0x2e:0x290
hwmon9 = nct6799
```

### 6.4 hwmon 快照

在内存风扇源设为 CPU 温度时：TSI0=45.9°C、PECI Agent0=45°C、SYSTIN=36°C、CPUTIN=33°C、AUXTIN0..5=31/4/20/26/40/23°C，PCH*=0；无 DDR 温度。`fan5=1698 rpm`、`pwm5=124`（约 49%），所有 `pwmN_enable=5`。`temp_sel` 是通道号索引，不能直接当 SRC 编码。

### 6.5 决策性实验

- 数据源=CPU 温度：转速正常响应。
- 数据源=内存温度：低档恒定约 941 rpm。
- 全速：正常。

### 6.6 前置实验（2025-09-04）

**实验 1：Virtual_TEMP 值寄存器定位，成功。**

- NCT 页 `0x0c` reg `0x36` 是内存风扇 Virtual_TEMP 值寄存器。
- 写 30°C（`0x1e`）后稳定 5 秒：`pwm5=76`、`fan5=1031 rpm`。
- 写 40°C（`0x28`）后稳定 5 秒：`pwm5=101`、`fan5=1326 rpm`。
- 确认编码为 `°C×1`。
- 页 `0x0c` 的 `0x3e/0x40/0x42/0x44/0x46/0x48/0x4a` 为 `0a`，对应 7 个风扇通道的 Virtual_TEMP 源选择寄存器；`0x20–0x2b` 为 `4b×6 + 03 04 05 06 07 10`。
- `0x183c` 写回序列中的 `0x36` 命中内存风扇，其他候选页/寄存器无响应。
- 脚本：`ref/scripts/exp1_virtemp_probe.py`。

**实验 2：SMBUSMASTER 硬件探测，南桥路径可用，子方案 C 排除。**

- 南桥 SMBus `0xb00` 读取从 `0x53`、命令 `0x31` 得到 `(96, 2, 608)`，换算为 38.0°C。
- NCT 自身 `SMBUSMASTER` 排除：LDN 枚举基址无效；`NCT6796_REG_TEMP_ALTERNATE` 中 src8/9 对应寄存器为 0；固件也绕道南桥读取。
- 芯片确认：chip id `0xd8 0x02` = NCT6796D-S。
- 脚本：`ref/scripts/exp2_smbus_probe.py`。

## 7. 环境备注

- 系统：Arch Linux。
- Ghidra 12.1.2 与 JDK26 不兼容，脚本编译失败；使用 objdump + flat-image，相关环境摩擦已记录在 `/home/Si/.pi/agent/PAPERCUTS.md`。
- 系统无 `xxd`，使用 `od`。
- BIOS 更新文件位于 `AfuEfix64.efi`（`MSiCraftB850AIGA16/Shell Update BIOS/EFI/BOOT/`）；可能存在 ROM 签名校验，尚未验证。

## 2026-09-04 工作流规划

- 已检查项目：`patch/linux/`、`patch/windows/` 尚为空；Rust/cargo 和 systemd 可用；现有 Python 实验脚本作为硬件访问参考。
- 已编写 `WORKFLOW.md`：确定 Linux 首版采用零运行时依赖的 Rust 单二进制 + systemd unit，先完成单次读写闭环，再进入 2 秒周期服务。
- Linux 首版边界：只读南桥 SMBus `0xb00` 的 SPD `0x53`/cmd `0x31`，只写 NCT 页 `0x0c` reg `0x36`；不修改曲线、温度源、BIOS 或内核驱动。
- 下一步：创建 `patch/linux` Cargo 工程，实现 `port.rs`、`smbus.rs`、`nct.rs` 和带 `--once` 的最小验证程序；读取所有已知 DIMM 地址并取有效读数最高值后喂入 NCT；实机 root 测试由机主执行。

## 2026-09-04 方案复核

- 独立审查确认寄存器主链路和单地址 SMBus transaction 基本成立，但长期 raw `/dev/port` 方案仍需先验证与 Linux SMBus/NCT 驱动的并发竞争。
- 已明确地址表示：Rust 使用 7-bit 地址 `0x53/0x52/0x51/0x50`，写入 HST 的读地址字节为 `0xa7/0xa5/0xa3/0xa1`；固件的 `0xa6/0xa4/0xa2/0xa0` 是候选写格式地址。
- 项目方案与固件行为已区分：固件是首个成功地址回退；项目首版计划轮询全部候选地址并取最高值，但仍需实机确认地址映射和空槽状态。
- 已修正失效语义：服务停止不会自动回到 941 rpm，Virtual_TEMP 通常保持最后一次写入值；必须在常驻服务前定义 stale/fail-safe 策略。
- 复核发现的阻塞项已写入 `WORKFLOW.md` 阶段 0；在并发、错误状态和失联行为确认前，不进入长期 2 秒部署。

## 2026-09-04 Linux 补丁初版实现

- 环境修正：WORKFLOW.md 所述 "cargo、rustc 均已找到" 不实——只有 rustup shim，无已安装工具链；已执行 `rustup default stable`（rustc 1.98.1）。
- 已创建 `patch/linux/`：Cargo 工程（零运行时依赖，edition 2021），`src/port.rs`（`/dev/port` 单字节读写，逐次校验长度）、`src/smbus.rs`（HST word read，候选 7-bit 地址 `0x53/0x52/0x51/0x50`，BUSY 外任意状态位置位即判失败，100ms 超时；换算 `((raw<<3)>>5)*25/100`，有效范围 0–120°C）、`src/nct.rs`（页 `0x0c` 选择 + reg `0x36` 写入 + 读回校验）、`src/main.rs`（`--once` 单次验证 / 2s 常驻循环；曾发现的 DIMM 本轮缺失则整轮跳过写入；连续失败按 10 倍数降频日志）、systemd unit、README（构建/实机验证/安装/回滚）。
- 质量门：`cargo fmt`、`cargo check`、`cargo test`（2 项：raw=608→38°C、边界与负温拒绝）、`cargo clippy` 全部通过；`cargo build --release` 产出二进制。
- 决策：温度范围定 0–120°C；负温度（raw 高位为符号样式）换算后越界拒绝，不写入；`gitignore` 追加 `patch/linux/target/`，`Cargo.lock` 纳入版本管理。
- 待办更新：等待机主按 `patch/linux/README.md` 做阶段 B `--once` 实机验证（含 0x50–0x53 与两个 spd5118 hwmon 对应关系、空槽返回状态、与内核驱动的并发确认）；通过后再部署阶段 C 常驻服务。
- 审查遗留（不阻塞实机，长期部署前处理）：`known` DIMM 集合只增不减——空槽若偶发一次假成功会永久卡死写入；候选缓解为空槽状态实测后改为"连续 2 次成功才进 known"或"连续 N 轮缺失移出 known"。

### 阶段 B 实机第 1 轮（2026-09-04，commit c100abf）

- `--once` 运行：四地址全部失败——`0x53/0x51` sts=`0x02`，`0x52/0x50` sts=`0x06`（KILL/CRC 样式错误位）；程序正确拒绝写入（无样本不写），基线 `fan5=946 pwm5=55` 未受影响。行为符合设计。
- 矛盾点：同一 OS 上 `exp2_smbus_probe.py` 同款序列历史上成功（38.0°C）。待判定是环境变化（内核 `i2c_piix4`/`spd5118` 占用 SMBus 控制器，对应 WORKFLOW 阶段 0 并发阻塞项）还是 exp2 现在也同样失败。
- 判定实验（交机主）：重跑 exp2；列出 PCI SMBus 驱动绑定、i2c 适配器、hwmon 清单与 spd5118 读数。若 exp2 同样失败 → 按 WORKFLOW 阶段 0 第 2 条优先切换 sysfs 温度后端（spd5118 已在 hwmon5/7），raw SMBus 降为 fallback。

### 阶段 B 实机第 2 轮与后端切换（2026-09-04）

- 对照实验（同机同时段）：`exp2_smbus_probe.py` 的 raw `0xb00` 事务**成功**（`0x53` d0=192 d1=2 raw=704 → 44.0°C）。控制器未被内核独占，Rust 版失败不是并发阻塞。
- 定位：差异在我方错误位判定。HST_STS 实测模式——`0x53/0x51` → sts=`0x02`，`0x52/0x50` → sts=`0x06`；结合 exp2 在 `0x53` 拿到有效数据、spd5118 恰好两个传感器：`bit1(0x02)` 是成功完成也会置位的位，`0x04`(CRC) 才标记无设备。结论供 Windows raw 实现复用：致命位至少含 `0x04`，`0x02` 须容忍。
- 环境事实：`i2c_piix4`（piix4_smbus，端口 0xb00/0xb20）绑定 FCH SMBus Controller `1022:790b`；`spd5118` 内核模块维护两根 DIMM（hwmon4=41.25°C、hwmon6=43.25°C）。
- 决策（WORKFLOW 阶段 0 第 2 条落地）：Linux 读取后端切换为 spd5118 sysfs（新增 `src/sysfs.rs`，按 hwmon name 扫描 `temp*_input`，毫摄氏度四舍五入、0-120°C 范围拒绝，负值/-ENODATA 视为读取失败→本轮不完整）；删除 raw SMBus 模块 `smbus.rs`。写入后端不变（页 0x0c reg 0x36 + 读回校验）。known 集合改为 hwmon 目录，目录消失（模块重载换号）自动移除重新学习。
- 质量门：cargo test 2 项（舍入/范围）过，clippy 零告警，release 构建成功。待实机第 3 轮：`--once` 应读到两个传感器并写入最高温（预期 ~43°C），对照 pwm5/fan5。
- 子代理审查（sysfs 版）：发现 1 项回归——参数解析复用了同一迭代器导致非法参数被静默接受（`any` 耗尽后 `find` 失效），已改为 collect 后判定并实机外验证 `foo --once`/`--bogus` 均 exit 2；known 保活增加 name 复核（`is_sensor`，防 hwmon 号复用）；README"不写 0°C"措辞澄清。LOW：LOG 中 hwmon5/7 与 hwmon4/6 编号为历史会话演进，非矛盾。

### 阶段 B 实机第 3 轮——sysfs 后端闭环通过（2026-09-04）

- `--once`：发现 hwmon6/hwmon4 两个 spd5118 传感器，samples=[44, 41]，写入最高值 44°C 读回一致，exit=0；基线 `fan5=944 pwm5=55` → 写后 `fan5=1571 pwm5=111`，单调响应与历史曲线吻合（30°C→76/1031、40°C→101/1326）。
- 结论：Linux 读取（spd5118 sysfs）+ 写入（页 0x0c reg 0x36）单轮闭环实机验证通过。BIOS 内存风扇源保持 `0x0a` 未动。
- 下一步（阶段 C 常驻服务前需先定）：nct6775 并发访问 NCT 口的实机确认；stale 策略（spd5118 长期读取失败时的行为）；unit 的 `ProtectSystem=strict` 下服务实际能否读 sysfs/写 /dev/port。
