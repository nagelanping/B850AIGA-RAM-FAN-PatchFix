# Patch 工作流：Linux Rust 优先

## 1. 目标与边界

本项目的第一交付目标是 Linux 用户态服务：在 BIOS 保持“内存温度”数据源不变的情况下，持续把 DIMM 温度喂给 NCT6796D 的 `Virtual_TEMP`，使 `FAN5=MEM_FAN` 按原有 BIOS 曲线运行。

固定链路：

```text
spd5118 hwmon sysfs（内核经南桥 SMBus 读取 DIMM）
        ↓
取全部有效传感器中的最高温度
        ↓
NCT 页 0x0c / reg 0x36（°C × 1）
        ↓
NCT 智能风扇算法 → FAN5
```

本阶段不做：

- BIOS/SMM 修改或刷写；
- 修改风扇曲线、模式或温度源选择；
- 修改 `nct6775` 内核驱动；
- Windows 实现；
- 自定义配置协议、GUI 或后台守护进程框架。

## 2. 当前项目检查结果

- `patch/linux/` 已完成 Rust/systemd 实现；`patch/windows/` 尚为空。
- 已有可运行的 Python 参考脚本：
  - `ref/scripts/exp1_virtemp_probe.py`：确认 NCT 页 `0x0c` reg `0x36`，并验证 30°C/40°C 会改变 PWM；
  - `ref/scripts/exp2_smbus_probe.py`：确认南桥 SMBus `0xb00` 可读取 DIMM 温度；
  - `ref/scripts/sio_probe.py`、`sio_probe2.py`：用于只读探测和实机对照。
- Rust 工具链可用：`cargo`、`rustc` 均已找到。
- systemd 可用，目标部署为普通 system service。
- 读写 `/dev/port` 需要 root 或等效的 `CAP_SYS_RAWIO`；实机测试命令仍由机主执行。
- 详细硬件事实、实验数据和风险约束以 `LOG.md` 为准，不在本文件重复推翻。

## 3. Linux patch 架构

采用一个 Rust 二进制 + 一个 systemd unit。优先少依赖、少进程、少状态。

建议目录：

```text
patch/linux/
├── Cargo.toml
├── Cargo.lock                  # 首次 cargo build 后提交
├── src/
│   ├── main.rs                 # 参数、日志、周期循环、退出处理
│   ├── port.rs                 # /dev/port 的 pio read/write
│   ├── sysfs.rs                # spd5118 hwmon 温度读取
│   └── nct.rs                  # NCT 页选择与 Virtual_TEMP 写入
├── ram-fan-virtual-temp.service
└── README.md                   # 构建、安装、回滚、验证
```

不引入 async runtime、CLI framework、日志框架或硬件访问 crate。标准库足够：

- `std::fs::File` + `FileExt::{read_at, write_at}` 访问 `/dev/port`；
- `std::time::{Duration, Instant}` 控制定时；
- `std::thread::sleep` 实现 0.5 秒周期；
- `eprintln!` 输出 journald 可收集的日志。

### 3.1 `port.rs`：最小端口后端

封装 `/dev/port`，只暴露：

```rust
read_u8(port: u16) -> Result<u8>
write_u8(port: u16, value: u8) -> Result<()>
```

实现要求：

- 启动时打开 `/dev/port` 一次，循环复用同一个文件描述符；
- 每次 `read_at`/`write_at` 必须检查返回长度是否为 1；
- 错误使用 `io::Error` 向上传递，不吞掉硬件访问失败；
- 不提供任意扫描、批量写入或自动探测接口。

### 3.2 温度读取：优先使用 spd5118 sysfs

Linux 实现读取 `/sys/class/hwmon/hwmon*/name` 为 `spd5118` 的设备及其
`temp*_input`，由内核 `i2c_piix4` 负责 SMBus 访问。用户态不再直接抢占 `0xb00`。
具体 raw SMBus 序列和状态位结论保留在 `LOG.md`，供 Windows 方案参考。

历史 raw SMBus 方案曾按以下方式实现：

首版不只读一个槽位，而是轮询固件已确认的候选 SPD 设备地址，收集所有有效温度。这里统一使用 SMBus **7-bit 地址**：`[0x53, 0x52, 0x51, 0x50]`；写入 HST 的读地址字节分别是 `[0xa7, 0xa5, 0xa3, 0xa1]`。`0xa6 → 0xa4 → 0xa2 → 0xa0` 是固件内部使用的候选写格式地址，不直接作为 Rust API 的地址类型。

每个地址执行一次完整 HST word-read：

1. `base + 0x00` 写 `0xff` 清状态；
2. `base + 0x04` 写 `((addr7 << 1) | 1)`；
3. `base + 0x03` 写命令 `0x31`；
4. `base + 0x02` 写 `0x4c` 启动 word read；
5. 轮询 `base + 0x00`，等待 BUSY 清除，超时则该地址失败；
6. 检查完整的、经实机确认的错误状态掩码；
7. 读取 `base + 0x05`、`base + 0x06`，组成小端 `u16` 并换算温度。

固件实际采用“首个成功地址”回退；本项目新增“轮询全部候选地址并取最高值”策略，不能将后者描述为固件行为。不存在设备或读失败的地址只记录为无效，不影响其他地址；只有所有地址都失败时，本轮才不写 NCT，继续等待下一轮。

温度换算必须使用整数运算，避免浮点和溢出歧义：

```text
scaled = (raw << 3) >> 5
celsius = (scaled * 25) / 100
```

`raw`、`scaled` 和中间乘积使用至少 `u32`。每个结果先检查合理 DIMM 温度范围，再加入有效样本集合。范围、符号语义和 SMBus 完整错误位必须在首轮实机验证前定下来。

**温度汇总策略：首版取所有有效 DIMM 温度中的最高值。** 最高温度直接驱动 Virtual_TEMP，能避免某一条内存过热时被平均值掩盖，且实现和验证最简单。若某个已发现的 DIMM 本轮读取失败，应将本轮视为不完整并保留上一次完整汇总值，不能直接用剩余低温样本降速。

最高值与平均值加权可作为后续校准选项，但不放入首个闭环版本：`T = round(α × max + (1-α) × average)`。只有实测表明最高值导致风扇波动过大时，才增加固定的 `α`（例如 0.75）配置并重新验证。

### 3.3 `nct.rs`：写入 Virtual_TEMP

只实现两个操作：选择页和写值。

选择 bank/page 时严格保留固件序列，不能覆盖高 4 位：

```text
outb(0x4e, 0x295)
v = inb(0x296)
outb((v & 0xf0) | (page & 0x0f), 0x296)
```

写入：

```text
select_page(0x0c)
outb(0x36, 0x295)
outb(celsius as u8, 0x296)
```

安全边界：

- 不写 bank `0x09` 的曲线和源选择寄存器；
- 不修改模式、PWM、温度点或任何非 `0x36` 寄存器；
- 默认每轮先读 SMBus，成功后才写 NCT；
- 温度读取失败时保持上一次**完整汇总值**，不写 0°C，不写猜测值；必须定义 stale 超时后的失联策略（告警、退出或写入已验证的保守高温），不能假设服务停止会自动让风扇回到 941 rpm。

### 3.4 `main.rs`：服务循环

最小主循环：

```text
打开 /dev/port
可选：启动时写入一次并记录结果
loop:
    读取 DIMM 温度
    校验温度
    写 Virtual_TEMP
    记录成功/失败
    sleep(0.5s)

建议行为：

- 默认周期 0.5 秒；使用常量，不增加命令行配置。
- 保持零依赖首版不编写 signal handler：systemd 的 SIGTERM 直接终止进程，内核自动关闭 fd；不做退出时“恢复值”写入。若以后需要优雅停机逻辑，再单独引入 Unix signal 依赖；
- 单次 SMBus 失败记录 warning，继续下一轮；连续失败不要刷屏，可做简单计数后按倍数降频日志；
- NCT 写失败同样继续重试；启动时权限或 `/dev/port` 打开失败应直接退出，让 systemd 报错；
- 日志至少包含读到的温度、写入值、错误类型和连续失败次数，便于与 `hwmon9/pwm5/fan5_input` 对照。

## 4. systemd 部署设计

`ram-fan-virtual-temp.service`：

```ini
[Unit]
Description=Feed DIMM temperature to NCT Virtual_TEMP for RAM fan
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/ram-fan-virtual-temp
Restart=on-failure
RestartSec=2
User=root
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true

[Install]
WantedBy=multi-user.target
```

首版不要加入会阻止 `/dev/port` 或降低硬件访问能力的 sandbox 选项；安装后逐项验证。`ProtectSystem=strict` 只影响文件系统写入，不应影响 `/dev/port`，但必须实机确认。若未来改为非 root 用户，再评估 `CAP_SYS_RAWIO` 和设备访问规则，不能凭空假设可行。

安装/回滚步骤写入 `patch/linux/README.md`，基本流程：

```bash
cargo build --release
sudo install -Dm755 target/release/ram-fan-virtual-temp /usr/local/sbin/ram-fan-virtual-temp
sudo install -Dm644 ram-fan-virtual-temp.service /etc/systemd/system/ram-fan-virtual-temp.service
sudo systemctl daemon-reload
sudo systemctl enable --now ram-fan-virtual-temp.service
```

回滚只停止并 disable 服务，再删除二进制和 unit；不涉及 BIOS 恢复。停止服务不会撤销最后一次写入，`0x0c:0x36` 通常会保持到 NCT 复位/重启；README 必须明确这一点。

## 5. 实施顺序

### 阶段 0：先消除长期运行阻塞项

1. 确认 `0x50–0x53` 与两个 `spd5118` hwmon 读数的对应关系；确认空槽的返回状态。
2. **已完成**：现有 Linux SMBus 驱动通过 `spd5118` sysfs 提供所需温度，Linux 使用 sysfs。
3. **已完成（短时实测）**：nct6775 并发观察无毛刺；用户态 SIO 多步序列仍无法与内核访问原子化，保留理论竞态风险。
4. **已完成（raw 参考）**：实测确认 bit1=`0x02` 可出现在有效事务中，`0x04` 为无设备/CRC 样式错误；Linux 不使用该 raw 路径。
5. **已完成**：失败只跳过写入并告警，不设超时回退温度。

### 阶段 A：可测试的纯逻辑

1. 创建 Cargo binary，使用 Rust 2021 edition，保持零运行时依赖。
2. 实现温度换算函数和边界检查。
3. 为换算保留一个 `#[cfg(test)]` 单元测试：至少覆盖已知 `raw=608 → 38°C`、边界值和异常值。
4. 用 `cargo fmt`、`cargo check`、`cargo test`。

### 阶段 B：硬件访问封装

1. 实现 `port.rs`，先只做 `/dev/port` 单字节读写。
2. **已完成**：实现 `spd5118` sysfs 温度读取。
3. 实现 NCT 页 `0x0c`、reg `0x36` 写入，写后读回确认。
4. 增加 `--once` 验证入口，先完成“一次读取→汇总→一次写入”，避免直接进入长期服务循环。
5. 由机主执行 root 实机测试；每次测试前记录原值和 `hwmon9/pwm5/fan5_input`。
6. 验证四个候选地址、两个已知 DIMM 的对应关系，以及取最高值时单个传感器暂时失败的处理。

### 阶段 C：闭环服务

验收前提：阶段 B 的单次读写已成功，且写入 30°C/40°C 的响应与 `LOG.md` 中历史数据一致。

1. **已完成**：加入 0.5 秒循环和 systemd unit。
2. **已完成**：安装和回滚文档。
3. **已完成**：服务启动后曲线响应。
4. **部分完成**：已完成单轮和约 2 分钟并发观察；内存加压动态测试未形成有效升温数据。
5. **已定案**：停止服务不清除最后值；失败只跳过写入并告警，不写回退温度。
6. **待补**：系统重启后不打开 BIOS 曲线页面，确认服务自动恢复。

### 阶段 D：审查与记录

1. 运行格式化、静态检查、单元测试。
2. 检查 diff，确认只写 `0x0c:0x36`，没有误写曲线或源选择。
3. 交由子代理独立审查寄存器、端口、换算、失败行为和与 `LOG.md` 的一致性。
4. 机主完成 root 实机验证后，把命令、结果、硬件环境和未解决问题写入 `LOG.md`。
5. 只有测试和审查均通过，才提交声明式 commit。

## 6. 验收标准

### 功能

- 服务能从 `spd5118` hwmon sysfs 读到有效 DIMM 温度（内核底层使用南桥 SMBus）。
- 写入 NCT 页 `0x0c` reg `0x36` 后，读回值等于摄氏度整数。
- `pwm5`/`fan5_input` 随 DIMM 温度和 BIOS 曲线变化，而不是固定约 941 rpm。
- BIOS 内存风扇源仍为 `0x0a`，曲线和模式未被服务修改。
- 服务重启、系统重启后均能自动工作，不需要打开 BIOS 页面。

### 安全与可靠性

- 无权限、sysfs 读取错误和 NCT I/O 错误均可观察；服务不会写入伪造温度。
- 温度异常时不执行越界的 `u8` 写入。
- 停止服务不刷写、不持久化、不改变 BIOS 设置。
- 默认只改目标值寄存器，风险范围小且重启可恢复。

### 交付物

- `patch/linux/Cargo.toml`
- `patch/linux/Cargo.lock`
- `patch/linux/src/*.rs`
- `patch/linux/ram-fan-virtual-temp.service`
- `patch/linux/README.md`
- `LOG.md` 中的实机验证记录

## 7. 未决问题与升级条件

- Linux 多 DIMM 读取和取最高值已由 spd5118 sysfs 闭环验证。
- NCT SIO 多步序列与 nct6775 的原子协调尚未实现；若出现偶发冲突，应改用内核协调接口，不在用户态增加假锁协议。
- `/dev/port` 是否适合长期部署需在实际内核和权限配置下确认；若不稳定，再评估写一个最小内核驱动，而不是先引入复杂框架。
- Windows 方案和 SMM 方案不阻塞 Linux 首个可用版本。
