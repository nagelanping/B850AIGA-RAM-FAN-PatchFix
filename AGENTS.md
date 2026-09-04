# AGENTS.md

## 项目简介

铭瑄 MAXSUN MS-iCraft B850 AIGA 主板"内存风扇自定义曲线重启后失效"问题的修复工程。根因：固件把内存风扇温度源配为 NCT6796D 的 `Virtual_TEMP`（src 0x0a），该通道无硬件数据、必须软件喂值，而固件只在 BIOS 打开曲线页时才喂值，重启后不再喂值 → 风扇固定在低档（941rpm）。

**修复方向（已定案）**：软件补丁优先 —— Linux systemd 服务 + Windows 驱动，双系统执行同一喂值逻辑：每 ~2s 读南桥 SMBus → 写 NCT 页 0x0C reg0x36。SMM 固件补丁仅备选（需刷写，变砖风险）。

## 关键已确认数据（勿重新调查，直接使用）

- **Virtual_TEMP 值寄存器 = NCT 页 0x0C、reg 0x36**，温度编码 **°C×1**（写 0x1e=30°C）
- 实测响应：写 30°C → pwm5=76/1031rpm；写 40°C → pwm5=101/1326rpm
- **内存温度来源 = 南桥 SMBus 基址 0xb00**，从地址 0x53（0xa6/0xa7 读写位）、命令 0x31、读 2 字节，换算 `(raw<<3>>5)*25/100`；OS 下已验证可读（38.0°C）
- NCT SIO 访问端口对 **0x295/0x296**；通道选择：`outb(0x4E,0x295); v=inb(0x296); outb((v&0xF0)|code,0x296)`，内存风扇 bank code=0x09
- 芯片：NCT6796D-S（chip id 0xd802）；风扇头 FAN5=MEM_FAN
- NCT 自身 SMBus 未物理连 DIMM（子方案 C 已排除）；NCT 硬件 18 路温度源无 DDR 温度

完整证据链与逆向档案在 **`LOG.md`**（12 节，含反汇编地址、实验数据、待办）。开始工作前先读它。

## 目录结构

- `LOG.md` — 唯一进度/档案文件。**所有任务状态、新发现、待办都写在这里**，AGENTS.md 不记进度
- `patch/linux/`、`patch/windows/` — 补丁实现目录（目前为空）
- `ref/` — 逆向参考（只读）：
  - `ref/bios/` — BIOS 镜像、UEFIExtract 解包、report/guids
  - `ref/disasm/` — M351/SkSmartFanProtocol/SkSmartFanCtrlPei/Setup 反汇编（`dis_M351.txt` 34.8 万行）
  - `ref/mods/` — 固件模块二进制（M351.bin、SkSmartFanCtrlPei.bin 等）
  - `ref/kernel/` — `nct6775-core.c`（NCT 内核驱动源码，寄存器表权威参考）
  - `ref/scripts/` — 实机探测/实验脚本（sio_probe.py、exp1_virtemp_probe.py、exp2_smbus_probe.py 等）
  - `ref/misc/` — TE→flat 映像等
- `tmp/` — 临时物（ghidra_proj 等，gitignore 排除，不存放有价值内容）

## 实机操作（重要约束）

- **本机是 Arch Linux 实机，Agent 会话无终端**：需要 root 的 `/dev/port` 访问必须由**机主执行**（sudo 需密码，Agent 无法交互输入）。交付命令清单，不自行执行
- nct6775 驱动**每次重启后挂载丢失**，需先 `sudo modprobe nct6775`；hwmon9 出现后才有 fan/pwm/temp 读数
- 常用命令（仓库根目录执行）：
  - 探测：`sudo python3 ref/scripts/sio_probe.py`
  - 值寄存器写测：`sudo modprobe nct6775 && sudo python3 ref/scripts/exp1_virtemp_probe.py`
  - SMBus/硬件探测：`sudo python3 ref/scripts/exp2_smbus_probe.py`
- 写 SIO 寄存器可逆（重启 BIOS 恢复），但改前记录原值；刷 BIOS 有变砖风险，需谨慎决策

## 已知陷阱

- 路径已从旧 `tmp/` 迁至 `ref/`，LOG.md 中的路径均已更新，按其中的路径使用
- 温度源 sysfs `temp_sel` 是通道号索引，不是 SRC 编码，勿混淆
- `SkSmartFanSetupData` 变量在 efivarfs 不存在，不要假设能从 OS 侧持久化
- Ghidra 12.1.2 与 JDK26 不兼容（脚本编译失败，见 `/home/Si/.pi/agent/PAPERCUTS.md`），用 objdump + flat-image
- 无 xxd，用 od

## 工作流

1. 任务前：阅读文档 `LOG.md` 和 `WORKFLOW.md`（状态、待办、已有证据），确认不重复已确认的工作
2. 完成后：更新文档（新增发现/实验数据/待办变更）
3. 提交前：见下方强制审查规则
4. 编辑代码后：如有新增可 grep 的符号/陷阱，同步维护 `AGENTS.md`

## 强制审查规则

**一项任务（主要是 patch/ 下的修改）确认完成后，必须交给子代理（subagent）独立审查，审查确认无误后才允许提交，commit 时格式使用声明式提交。** 审查重点：寄存器/端口/温度换算正确性、双系统行为一致性、与 LOG.md 已确认数据不矛盾、是否引入破坏性副作用。
