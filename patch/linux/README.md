# ram-fan-virtual-temp（Linux 补丁）

把 DIMM 温度持续喂给 NCT6796D 的 `Virtual_TEMP`（SIO 页 `0x0c` reg `0x36`），
使 `FAN5=MEM_FAN` 在 BIOS 数据源为"内存温度"时按 BIOS 曲线运行。

每约 2 秒：读内核 `spd5118` hwmon（sysfs）各 `temp*_input` → 取有效读数最高值 → 写入。
只写这一个寄存器；不修改曲线、温度源、模式、BIOS 或内核驱动。

## 为什么读 sysfs 而不是自己发 SMBus

实机验证（LOG.md 阶段 B 第 1/2 轮）：内核 `i2c_piix4` 绑定 FCH SMBus `0xb00` 并
持续维护 SPD5118 温度（两根 DIMM = 两个 `spd5118` hwmon）。用户态 raw 事务可行但
状态位语义苛刻（成功也置 `sts bit1`），且与内核并发无锁。按 WORKFLOW 阶段 0 第 2 条，
sysfs 是优先路径；raw SMBus 结论（含 sts 位数据）保留在 LOG.md 供 Windows 方案复用。

## 构建

```bash
cargo build --release
```

## 实机验证（阶段 B，先于服务部署）

需要 root（`/dev/port`）。测试前记录基线：

```bash
sudo modprobe nct6775           # 每次重启后需手动加载，出现 hwmon9
cat /sys/class/hwmon/hwmon9/{fan5_input,pwm5}
sudo target/release/ram-fan-virtual-temp --once   # 逐传感器打印读数与写入结果（cwd=patch/linux）
echo $?
cat /sys/class/hwmon/hwmon9/{fan5_input,pwm5}     # 对照转速变化
```

验收点：两个 `spd5118` 传感器全部读到合理温度；写入读回一致；
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
要恢复"失效基线"（约 941 rpm）必须重启，而不是只停服务。

## 失败行为

- 任一已发现传感器本轮读取失败：整轮跳过写入（保持上一次值），日志给出首个错误。
- 没有任何传感器（`spd5118` 未加载等）：本轮不写。
- hwmon 目录消失（模块重载换号）：从已发现集合移除并按新扫描结果重新学习。
- NCT 写失败或读回不一致：记 ERROR，下一轮重试。
- 读取失败时不写 0°C、不写猜测值；仅写入传感器实际回报的 0–120°C 读数。

## 已知未决（长期部署前确认）

- 与 `nct6775` hwmon 驱动并发访问 NCT `0x295/0x296` 的实机确认（读取路径已无竞争）。
`ProtectSystem=strict` sandbox 验证：并入阶段 C 部署自检（`systemctl status` + 日志），pre 测试版本本机执行。
- ~~stale 策略~~：已定案（机主 2026-09-04）——失败只跳过写入并告警，不设超时回退值（见 LOG 阶段 C 准备）。
