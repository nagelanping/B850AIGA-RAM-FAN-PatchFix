# ram-fan-virtual-temp（Linux 补丁）

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为“内存温度”时按 BIOS 曲线运行。
每约 2 秒：南桥 SMBus `0xb00` 轮询 SPD `0x53/0x52/0x51/0x50`（cmd `0x31`）→ 取有效读数最高值 → 写入。

只写这一个寄存器；不修改曲线、温度源、模式、BIOS 或内核驱动。

## 构建

```bash
cargo build --release
```

## 实机验证（阶段 B，先于服务部署）

需要 root（`/dev/port`）。测试前记录基线：

```bash
sudo modprobe nct6775           # 每次重启后需手动加载，出现 hwmon9
cat /sys/class/hwmon/hwmon9/{fan5_input,pwm5}
sudo target/release/ram-fan-virtual-temp --once   # 逐地址打印读数与写入结果（cwd=patch/linux）
echo $?
cat /sys/class/hwmon/hwmon9/{fan5_input,pwm5}           # 对照转速变化
```

验收点（对照 WORKFLOW.md §6）：至少两个 SPD 地址读到合理温度；写入读回一致；
`pwm5`/`fan5_input` 随温度变化（历史数据：30°C→76/1031rpm，40°C→101/1326rpm）。

## 安装为服务

```bash
sudo install -Dm755 target/release/ram-fan-virtual-temp /usr/local/sbin/ram-fan-virtual-temp
sudo install -Dm644 ram-fan-virtual-temp.service /etc/systemd/system/ram-fan-virtual-temp.service
sudo systemctl daemon-reload
sudo systemctl enable --now ram-fan-virtual-temp.service
journalctl -u ram-fan-virtual-temp -f
```

## 回滚

```bash
sudo systemctl disable --now ram-fan-virtual-temp.service
sudo rm /etc/systemd/system/ram-fan-virtual-temp.service /usr/local/sbin/ram-fan-virtual-temp
```

注意：停止服务**不会**清除最后一次写入的 `0x0c:0x36` 值——它会保持到 NCT 复位/重启。
要恢复“失效基线”（约 941 rpm）必须重启，而不是只停服务。

## 失败行为

- 单个 SMBus 地址失败：该样本丢弃；若曾发现过的 DIMM 本轮缺失，整轮跳过写入（保持上一次值）。
- 全部地址失败：本轮不写。
- NCT 写失败或读回不一致：记 ERROR，下一轮重试。
- 不写 0°C、不写猜测值。失联（stale）超时策略尚未定义（见 WORKFLOW.md 阶段 0）。

## 已知未决（阶段 0，长期部署前必须确认）

- 与内核 `i2c_piix4`/SPD5118 hwmon（`spd5118`，现有 hwmon5/hwmon7）并发访问 `0xb00` 的竞争。
- HST 状态寄存器完整错误位（当前实现：BUSY 外任意位置位即判失败）。
- `ProtectSystem=strict` 等 sandbox 项对 `/dev/port` 的实机影响。
