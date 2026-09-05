# Windows Patch 工作流

## 1. 目标

为 MAXSUN MS-iCraft B850 AIGA 修复 Windows 下重启后内存风扇曲线失效：不修改 BIOS、不刷写固件、不改曲线配置，只持续把 DIMM 温度写入 NCT6796D 的 `Virtual_TEMP`，让 BIOS 已配置的 `FAN5=MEM_FAN` 曲线继续工作。

固定数据链路：

```text
Windows 内核驱动
  ├─ 南桥 SMBus 0xb00 → SPD5118 DIMM 温度
  └─ NCT SIO 0x295/0x296 → 页 0x0c、reg 0x36（°C × 1）
                                  ↓
                         BIOS Smart Fan → FAN5
```

服务每 0.5 秒请求驱动执行一次完整喂值。读取失败或写入失败时跳过本轮，不写 0、不写猜测值，保持上一次成功值并记录告警。

## 2. 已确认事实（不要重新逆向）

| 项目         | 值                                                                                               |
| ------------ | ------------------------------------------------------------------------------------------------ |
| 主板 / BIOS  | MAXSUN MS-iCraft B850 AIGA；E1.6D 已验证                                                         |
| NCT 芯片     | 实测 chip id`0xd802`，NCT6796D-S / NCT6799D 兼容系列                                           |
| 内存风扇     | `FAN5=MEM_FAN`，bank/page code `0x09`                                                        |
| 温度源       | bank`0x09` 的 reg `0x00` = `0x0a`（`Virtual_TEMP`）                                      |
| 写入目标     | NCT page`0x0c`、reg `0x36`                                                                   |
| 温度编码     | 摄氏度整数，`0x1e` = 30°C                                                                     |
| SMBus 基址   | `0xb00`（实机 FCH SMBus）                                                                      |
| SPD 设备     | 7-bit 地址优先轮询`0x53, 0x52, 0x51, 0x50`；命令 `0x31`；word read                           |
| 温度换算     | `scaled=(raw << 3) >> 5`；`celsius=(scaled * 25) / 100`                                      |
| SMBus 成功位 | `HST_STS bit1 (0x02)` 可出现在成功事务；`0x04` 是无设备/CRC 样式错误，不能把 `0x02` 当失败 |
| 页选择       | `outb(0x4e, 0x295)`；读 `0x296`；写 `(old & 0xf0) \| page`                                  |
| 周期         | 0.5 秒                                                                                           |

实测响应：写 30°C → `pwm5=76`、约 1031 rpm；写 40°C → `pwm5=101`、约 1326 rpm。Linux 已完成 sysfs 读取 + NCT 写回闭环；Linux 版本和详细证据在 `archive/0.1/linux/`，历史逆向材料在 `ref/`。

## 3. Windows 方案边界

### 3.1 首选架构

采用 **KMDF 内核驱动 + Windows Service**，不依赖 WinRing0、InpOut32 等第三方环路驱动。原因是 Windows 用户态不能直接可靠执行 I/O port，第三方驱动的签名、兼容性和安全边界不应成为正式交付物。

- 驱动负责：端口 I/O、SMBus 完整事务、状态位判断、温度换算、NCT 页选择和目标寄存器写回。
- 服务负责：启动/停止、0.5 秒周期、日志、`--once` 验证入口和错误重试。
- 驱动提供一个最小的 `IOCTL_RAM_FAN_FEED_ONCE`，一次调用完成“读取所有候选 DIMM → 校验 → 取最高温 → 写入并读回校验”。不要把多个裸端口操作暴露给服务。
- 驱动内部用单次调用锁住完整硬件序列，防止本驱动的并发请求交错；不能假设它能与其他硬件监控程序或固件访问自动原子化。
- 驱动只先实现并验证只读路径；在确认端口资源和 SMBus 所有权前，不进入写回和常驻服务。

WDM 仅在目标机无法使用 KMDF 时评估。禁止先实现 GUI、配置协议、可调周期、固件补丁或多种后端。

### 3.2 端口与 SMBus 实现要求

1. 不要在代码中无条件假设 SMBus 基址永远是 `0xb00`。只有绑定 AMD SMBus PCI 设备的 PnP upper-filter 在 `EvtDevicePrepareHardware` 收到 translated resource 后，才能使用该资源；`0xb00` 只作为当前已知诊断结果。
2. SMBus 与 NCT 是两个独立资源所有者：AMD PCI SMBus upper-filter 只持有 SMBus 资源；独立 `PNP0C02` NCT 设备若能合法绑定，才可持有覆盖 `0x295/0x296` 的 translated resource。不能由一个设备的资源推断另一个设备的端口权限。
2a. 当前非 PnP 控制设备没有 `EvtDevicePrepareHardware` 或 translated resource list。资源模型解决前，`FEED_ONCE` 必须在任何硬件访问前返回 `RAMFAN_FEED_HW_UNAVAILABLE`；不得安装测试或执行真实写回。
2b. 标准 SIO `0x2e/0x2f` 也必须单独出现在合法 translated resource 中并完成身份访问验证；不能用 `0x295/0x296` 或历史 `0xd802` 证据替代。
3. 每个 SPD 地址执行完整 HST word-read：清状态、写从地址读格式、写命令 `0x31`、启动 `0x4c`、轮询 BUSY、检查错误位、读取两个数据字节。
4. BUSY 超时、`0x04` 或其他明确错误、非法 raw、换算结果不在 `0..120°C` 均视为该地址失败。
5. 首轮先用 HWiNFO/只读 SMBus 结果建立“已安装 DIMM”与 SPD 地址的映射。空槽 NACK 不算故障；已安装地址的超时、总线错误或异常状态算该轮失败。只有所有已安装 DIMM 都成功才写 NCT；首版取最高值，不用剩余低温值降速。
6. NCT 写入严格只允许 page `0x0c` / reg `0x36`，写后读回必须一致；进入写入前保存当前页，成功或失败时尽力恢复。不得写 page `0x09` 的曲线或源选择寄存器。
7. 所有端口访问失败返回明确 NTSTATUS。SMBus 超时必须先执行有限的状态清理/中止并确认 BUSY 已清除；若控制器仍不可恢复，停止本驱动的写回并报告错误，不对共享控制器做未经验证的强制复位。
8. 驱动内部锁只覆盖本驱动；阶段 2 前必须停止会访问这些端口的 HWiNFO/同类工具并记录冲突观察。若外部并发导致事务不稳定，不得靠服务层加锁掩盖问题。

### 3.3 服务行为

- Windows Service 使用 SCM 注册，默认 `SERVICE_DEMAND_START` 开发测试，验收后再设为自动启动（Delayed Auto-Start 不是必需项）。服务打开设备失败时有限退避重试，不能依赖固定启动顺序。
- 默认启动即喂值；`--once` 完成一次 IOCTL 并返回：成功 `0`，硬件/读写失败 `1`，参数错误 `2`。
- 使用 Windows Event Log 或统一文本日志；至少记录驱动连接失败、有效温度样本、写入值、读回值、连续失败次数和恢复事件。成功日志不按 0.5 秒刷屏。
- 服务停止不清除 NCT 最后一次写入值；卸载不会修改 BIOS。卸载前明确告知该行为。
- 设备句柄、服务线程和驱动资源必须在停止路径正确关闭；不做退出时写“安全温度”的未经验证行为。

## 4. 实施阶段

### 阶段 0：切换到 Windows 后确认环境

机主在 Windows 记录：Windows 版本、Secure Boot 状态、内存条数量、设备管理器中的 SMBus 控制器、驱动签名策略和可用磁盘空间。安装 Visual Studio + Windows SDK + WDK，使用 x64 Debug 构建；不关闭 Secure Boot 作为默认前提。

先用只读工具确认：

- PCI FCH SMBus 资源是否为 `0xb00`；
- NCT 芯片 id 是否为 `0xd802`；
- BIOS 中 `FAN5` 温度源仍为“内存温度”且为 `0x0a`；
- HWiNFO 等现有工具能看到 DIMM 温度（仅作对照，不让多个工具写 NCT）。

### 阶段 1：驱动骨架和只读验证

创建 `patch/windows/` 的 KMDF x64 工程、INF、签名/测试安装脚本和最小服务工程。先实现设备创建、IOCTL 通路、硬件识别和只读 SMBus 温度请求；此阶段禁止写 NCT。

质量门：驱动能加载/卸载，服务能打开设备；错误状态可观察；不匹配硬件时拒绝工作；不会触碰风扇寄存器。

### 阶段 2：一次性写回闭环（资源模型通过后）

当前阶段 2只保留 `IOCTL_RAM_FAN_FEED_ONCE` 接口，资源模型未通过前立即返回 `RAMFAN_FEED_HW_UNAVAILABLE`，禁止安装、真实端口访问和写回。

先将驱动迁移为可接收 PnP 资源的模型：AMD SMBus 使用 PCI upper-filter；NCT 端口必须由独立、经确认可绑定的 `PNP0C02` 设备持有。两个设备均在 `EvtDevicePrepareHardware` 获得合法 translated resources 后，才恢复完整事务和 `--once` 写回。

资源模型确认后，保存 Secure Boot / BCD 原状；开发机先验证错误路径，再由机主在目标板执行：

1. 记录 `fan5/pwm5` 基线以及 NCT 原值；
2. 停止 Linux 或其他硬件监控写入方，避免同时访问 `0x295/0x296`；
3. 运行 `--once`，记录每个地址结果、最高温度、写入和读回值；
4. 对照 30°C / 40°C 历史响应，确认 `pwm5` 和 `fan5` 单调变化；
5. 结束后重启，确认 BIOS 曲线和温度源未被改写。

### 阶段 3：Windows 常驻服务

一次性闭环通过后启用 0.5 秒循环和 SCM 自动启动。验证服务重启、睡眠恢复和系统重启后，在不打开 BIOS 曲线页的情况下自动恢复喂值。连续采样 `pwm5/fan5`，同时观察服务日志；不要以单次风扇读数作为唯一结论。连续失败导致旧值过期时，首版暂不擅自写安全温度，必须把旧值保持作为明确的热安全风险记录。

动态验收至少包含：空槽、单 DIMM 读取失败、全部读取失败、SMBus 超时、NCT 读回不一致、服务停止/启动和短时内存加压。失败时应保持上次值，不应写 0°C。

### 阶段 4：审查、打包和记录

执行驱动静态检查、x64 Release 构建、服务自检和安装/卸载测试。提交前必须交子代理独立审查：端口、SMBus 状态位、温度换算、IOCTL 边界、NCT 写入范围、并发和失败策略，以及与 `LOG.md` 的一致性。机主完成目标机测试后，将版本、签名方式、命令、结果和未决风险写入 `LOG.md`，然后再制作发布包。启用 Secure Boot 的正式交付必须有 Microsoft Attestation/WHQL 等可信签名；否则只能交付明确标注的测试版。

## 5. 目录与交付物

首版保持最小结构：

```text
patch/windows/
├── driver/                 # KMDF 非 PnP 驱动、vcxproj（无 INF，SCM 直装）
├── service/                # Windows Service、--once、日志
├── build.ps1               # 可重复构建，不隐含关闭安全启动
├── install-test.ps1        # 测试签名安装，显式警告
├── uninstall.ps1
└── WINDOWS.md              # 目标机安装、验证、回滚
```

正式交付至少包含：签名驱动包、服务可执行文件、安装/卸载脚本、SHA-256、支持范围、已知限制和回滚步骤。没有合适签名时只能称为开发测试版，不宣称正式发布。

## 6. 验收标准

- 目标机重启后服务自动启动，不打开 BIOS 页面也能喂值；
- DIMM 温度读数与 HWiNFO/BIOS 对照在合理误差内；
- `Virtual_TEMP` 写入和读回一致，`pwm5/fan5` 按原 BIOS 曲线响应；
- 仅写 page `0x0c` / reg `0x36`，不改变曲线、模式或温度源；
- 读取失败不会写伪造温度，服务可重试且日志不过量；
- 驱动、服务可停止、卸载、回滚；不要求刷 BIOS；
- Secure Boot 和驱动签名限制被明确记录；测试签名版不列为普通用户正式安装方案。

## 7. 失败转向条件

- 若 Windows 无法在不引入不受信任第三方驱动的情况下获得安全的端口访问，暂停实现并记录原因；不要绕过签名策略交付。
- 若 SMBus 被 Windows 驱动独占或 raw HST 访问不稳定，优先研究 Windows 可用的标准 SPD/SMBus 内核接口；不在服务层增加轮询竞争、全局用户态锁或 GUI。
- 若无法获得合法的 NCT 端口资源/访问模型，或控制器超时后无法安全恢复，停在只读阶段，不实现写回。
- 只有 OS 驱动方案确认不可行，才重新评估 SMM 固件方案；刷 BIOS 必须另行批准、备份和签名校验。
