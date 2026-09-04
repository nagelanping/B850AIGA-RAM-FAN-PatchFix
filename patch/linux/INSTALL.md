# Linux 安装指南

本教程安装 `ram-fan-virtual-temp` systemd 服务。服务每 0.5 秒读取 `spd5118` hwmon 的 DIMM 温度，并将最高温度写入 NCT6796D 的 `Virtual_TEMP`（页 `0x0c`、reg `0x36`）。
本指南适用于 `B850AIGA-RAM-FAN-PatchFix_Linux.tar.gz` 发布包。用户不需要 Git、Cargo 或源码。

发布包目录应包含：

```text
./ram-fan-virtual-temp
./ram-fan-virtual-temp.service
./README.md
./INSTALL.md
./LICENSE
```

在终端进入你实际解压出的发布包目录。以下命令均在该目录中执行。

## 安装前检查

服务适用于 BIOS 中 `FAN5=MEM_FAN` 温度源设置为“内存温度”的情况。安装前不要修改 BIOS 风扇曲线、温度源或模式。

确认系统提供 `spd5118` 温度设备：

```bash
for h in /sys/class/hwmon/hwmon*; do
  [ "$(cat "$h/name" 2>/dev/null)" = spd5118 ] && \
    echo "$h: $(cat "$h/temp1_input" 2>/dev/null) m°C"
done
```

至少应显示一个 `spd5118` 设备和温度值。如果没有输出，先不要安装服务。当前版本依赖 Linux 内核提供的 `spd5118` hwmon 设备。

确认服务文件和二进制存在：

```bash
test -x ./ram-fan-virtual-temp
test -f ./ram-fan-virtual-temp.service
```

## 首次安装

服务需要 root 权限访问 `/dev/port`。执行：

```bash
sudo install -Dm755 \
  ./ram-fan-virtual-temp \
  /usr/local/sbin/ram-fan-virtual-temp

sudo install -Dm644 \
  ./ram-fan-virtual-temp.service \
  /etc/systemd/system/ram-fan-virtual-temp.service

sudo systemctl daemon-reload
sudo systemctl enable --now ram-fan-virtual-temp.service
```

`enable` 设置开机启动，`--now` 立即启动服务。

## 安装后检查

```bash
systemctl status ram-fan-virtual-temp.service --no-pager
systemctl is-enabled ram-fan-virtual-temp.service
sudo journalctl -u ram-fan-virtual-temp.service -n 30 --no-pager
```

预期状态：

```text
Active: active (running)
enabled
```

正常运行时不会每 0.5 秒输出成功日志。读取失败或写入失败时，日志会记录告警；服务会继续重试。

## 查看温度和风扇

`hwmon` 编号可能随系统启动或驱动重载变化。下面的命令按 `name` 查找 DIMM 温度，但当前主板的 `FAN5` 读数仍需要找到 `nct6799` 设备目录：

```bash
for h in /sys/class/hwmon/hwmon*; do
  name=$(cat "$h/name" 2>/dev/null)
  if [ "$name" = spd5118 ]; then
    echo "DIMM $h: $(cat "$h/temp1_input" 2>/dev/null) m°C"
  elif [ "$name" = nct6799 ]; then
    echo "NCT $h: fan5=$(cat "$h/fan5_input" 2>/dev/null) rpm pwm5=$(cat "$h/pwm5" 2>/dev/null)"
  fi
done
```

连续观察：

```bash
while sleep 10; do
  date +%T
  for h in /sys/class/hwmon/hwmon*; do
    name=$(cat "$h/name" 2>/dev/null)
    if [ "$name" = spd5118 ]; then
      echo "DIMM $h: $(cat "$h/temp1_input" 2>/dev/null) m°C"
    elif [ "$name" = nct6799 ]; then
      echo "NCT $h: fan5=$(cat "$h/fan5_input" 2>/dev/null) rpm pwm5=$(cat "$h/pwm5" 2>/dev/null)"
    fi
  done
done
```

按 `Ctrl-C` 停止观察。DIMM 温度升高较慢，BIOS 曲线在约 40°C 以下较平缓。

## 更新已安装版本

(可选)在新的发布包目录中执行。先备份当前二进制：

```bash
sudo cp -a \
  /usr/local/sbin/ram-fan-virtual-temp \
  /usr/local/sbin/ram-fan-virtual-temp.old
```

安装新文件并重启服务：

```bash
sudo install -Dm755 \
  ./ram-fan-virtual-temp \
  /usr/local/sbin/ram-fan-virtual-temp

sudo install -Dm644 \
  ./ram-fan-virtual-temp.service \
  /etc/systemd/system/ram-fan-virtual-temp.service

sudo systemctl daemon-reload
sudo systemctl restart ram-fan-virtual-temp.service
```

只替换文件不会更新正在运行的进程。必须执行 `restart`。

确认更新后的服务正在运行：

```bash
systemctl show ram-fan-virtual-temp.service \
  -p MainPID \
  -p ExecMainStartTimestamp \
  -p ActiveState \
  -p SubState
```

## 单次验证

`--once` 会读取温度并写入一次 `Virtual_TEMP`。常驻服务运行时，不要同时执行 `--once`，因为两者都会访问 NCT SIO 端口。先停止服务：

```bash
sudo systemctl stop ram-fan-virtual-temp.service
sudo modprobe nct6775
sudo ./ram-fan-virtual-temp --once
echo $?
```

退出码：

- `0`：读取完整且写入、读回校验成功；
- `1`：温度读取不完整或 NCT I/O 失败；
- `2`：命令参数错误。

验证结束后启动服务：

```bash
sudo systemctl start ram-fan-virtual-temp.service
```

## 停止和卸载

停止服务但保留开机启动配置：

```bash
sudo systemctl stop ram-fan-virtual-temp.service
```

停止服务并取消开机启动：

```bash
sudo systemctl disable --now ram-fan-virtual-temp.service
```

删除安装文件：

```bash
sudo rm -f \
  /etc/systemd/system/ram-fan-virtual-temp.service \
  /usr/local/sbin/ram-fan-virtual-temp \
  /usr/local/sbin/ram-fan-virtual-temp.old
sudo systemctl daemon-reload
```

停止服务不会清除最后一次写入的 `Virtual_TEMP` 值。该值通常保持到 NCT 复位或系统重启。

## 回滚更新

如果更新后的服务不能启动，使用更新前的备份：

```bash
sudo systemctl stop ram-fan-virtual-temp.service
sudo mv \
  /usr/local/sbin/ram-fan-virtual-temp.old \
  /usr/local/sbin/ram-fan-virtual-temp
sudo systemctl start ram-fan-virtual-temp.service
```

然后查看状态和日志：

```bash
systemctl status ram-fan-virtual-temp.service --no-pager
sudo journalctl -u ram-fan-virtual-temp.service -n 50 --no-pager
```

## 当前限制

- 依赖 Linux 内核提供 `spd5118` hwmon 温度；没有该设备时不写入回退温度；
- 服务以 root 运行，因为需要访问 `/dev/port`；
- NCT 多步 SIO 访问无法与 `nct6775` 内核访问原子协调。短时并发观察未发现问题，但理论竞态仍存在，实际不影响使用
