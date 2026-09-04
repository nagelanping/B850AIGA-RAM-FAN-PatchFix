# LOG — MS-iCraft B850 AIGA 内存风扇曲线修复

## 当前状态

- **Linux 0.1 已归档**：实现和完整 Linux 工作记录位于 `archive/0.1/linux/`；发布包仍在 `release/linux/`。
- **当前工作目标**：实现 Windows KMDF 内核驱动 + Windows Service，持续向 NCT `Virtual_TEMP` 喂入 DIMM 温度。
- **当前阶段**：Windows 开发前的工作流已建立；尚未创建 `patch/windows/` 实现。
- **修复路线**：优先 OS 软件补丁；不刷 BIOS。SMM 固件补丁仅在 OS 方案确认不可行后重新评估。

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

1. 切换 Windows 后记录版本、Secure Boot、内存条数量、SMBus 控制器资源和驱动签名状态。
2. 准备 Visual Studio、Windows SDK、WDK，建立 x64 KMDF Debug 工程和最小 Service。
3. 阶段 1：完成驱动加载/卸载、硬件识别、设备句柄和只读 SMBus IOCTL；此阶段禁止写 NCT。
4. 阶段 2：实现完整 `IOCTL_RAM_FAN_FEED_ONCE` 与 `--once`，目标机验证地址、温度、写入/读回及风扇响应。
5. 阶段 3：启用 0.5 秒常驻服务，验证服务重启、睡眠恢复和系统重启后自动恢复。
6. 阶段 4：完成空槽、单 DIMM 失败、全失败、超时、读回不一致、卸载回滚和内存加压测试。
7. 代码完成后交子代理独立审查；机主实机验证结果、签名方式、版本和风险写回本文件。

## 风险与禁止事项

- 不同时运行多个会写 `0x295/0x296` 的硬件监控/补丁程序；驱动内部锁不能解决外部程序竞争。
- 服务停止不会清除 NCT 最后一次写入值；不实现未经验证的退出“安全温度”写入。
- 不修改 page `0x09` 的曲线、模式、温度源或其他寄存器。
- 不修改 BIOS、UEFI 变量或 SMM；刷写 BIOS 必须另行批准并备份、校验。
- 如果 Windows 无法在可信签名和可接受安全边界内访问端口，停止扩展实现并记录原因，不绕过驱动签名策略交付。

## 2026-09-04 Windows 工作流轻量审查

- 结论：KMDF 驱动 + Windows Service 的方向可行，先只读、再一次性写回、最后常驻服务的顺序合理。
- 已在 `WORKFLOW.md` 补充阻塞前置：驱动 PnP/resource model、空槽 NACK 与已安装 DIMM 故障的区分、SMBus 超时清理、NCT 页恢复、外部并发限制及正式签名门槛。
- 旧值保持策略仍不写未经验证的安全温度；连续失败导致旧值过期属于明确热安全风险，须在目标机测试和发布说明中保留。


## 2026-09-04 AGENTS.md 双系统维护

- 根目录 `AGENTS.md` 已从单一混合约束维护为 Linux / Windows 两部分：Linux 维护边界、Windows KMDF + Service 开发边界、阶段门槛、实机权限和共享硬件访问风险。
- 保留既有项目规则和关键硬件事实，未按归档 `LOG.md` 的方式删减历史；Linux 归档继续保留完整记录。
- 子代理轻量审查通过：未发现与 `WORKFLOW.md` 或已确认硬件事实冲突；已同步温度范围、DIMM 地址映射/失败语义、Linux 部分验收状态和 Windows 服务启动策略。

## 参考资料

- `WORKFLOW.md`：Windows 当前实施和验收流程。
- `archive/0.1/linux/`：Linux 0.1 的完整 LOG、WORKFLOW 和实现背景。
- `ref/scripts/exp1_virtemp_probe.py`：Virtual_TEMP 目标寄存器与响应实验。
- `ref/scripts/exp2_smbus_probe.py`：南桥 SMBus DIMM 读取实验。
- `ref/scripts/sio_probe.py`、`sio_probe2.py`、`sio_dump.txt`：NCT/SIO 探测。
- `ref/disasm/`、`ref/mods/`：固件逆向材料。
- `ref/kernel/nct6775-core.c`：NCT 温度源和寄存器参考。
