# MS-iCraft B850 AIGA 内存风扇曲线修复

这个项目修复 MAXSUN MS-iCraft B850 AIGA / 铭瑄 B850 瑷珈 主板上的一个固件问题：重启后，`FAN5=MEM_FAN` 的内存风扇曲线不再跟随内存温度。
Linux 版本补丁已完成，Windows 版本正在开发中。

普通用户请直接使用 release 中的发布包。安装步骤见 `install.md`。

## 适用范围

已在以下硬件和 Linux 环境完成实机验证：

- 主板：MAXSUN MS-iCraft B850 AIGA；
- 风扇接口：`FAN5=MEM_FAN`；
- 风扇温度源：BIOS 中的“内存温度”；
- 内存温度设备：内核 `spd5118` hwmon；
- NCT 芯片：实测兼容 NCT6796D-S/NCT6799D；
- 系统：Linux，使用 systemd。

其他主板、其他 NCT 型号或没有 `spd5118` hwmon 的系统不在已验证范围内。

## 修复内容

BIOS 将内存风扇的温度源配置为 NCT6796D 的 `Virtual_TEMP`。这个通道没有硬件温度数据，固件只在 BIOS 打开内存风扇曲线页面时写入温度。系统重启后停止写入，风扇会回到低档。

Linux 服务每 0.5 秒读取内核提供的 DIMM 温度，取所有有效读数中的最高值，然后写入 `Virtual_TEMP`。BIOS 中的风扇曲线、温度源、模式和其他风扇设置不会被修改。

服务只执行以下操作：

```text
读取 spd5118 hwmon 温度
        ↓
取最高有效 DIMM 温度
        ↓
写入 NCT Virtual_TEMP（页 0x0c、寄存器 0x36）
        ↓
FAN5 按原有 BIOS 曲线运行
```

## 当前状态

Linux 版本是已实机验证的版本：

- 单次读取、写入和风扇响应已验证；
- systemd 常驻服务已验证；
- `ProtectSystem=strict` 下的服务访问已验证；
- 与 `nct6775` 同时运行时，短时观察未发现风扇读数毛刺。

NCT 的页选择、寄存器选择和数据写入由多个 `/dev/port` 操作组成，无法与 `nct6775` 内核访问原子协调。短时测试未复现问题，但该理论竞态仍然存在，实际不影响使用。

## 安装

1. 获取 release 发布包并解压；
2. 按 `INSTALL.md` 执行安装。

服务需要 root 权限访问 `/dev/port`。安装前不要修改 BIOS 风扇曲线或温度源。

## 停止和卸载

```bash
sudo systemctl disable --now ram-fan-virtual-temp.service
sudo rm -f /etc/systemd/system/ram-fan-virtual-temp.service
sudo rm -f /usr/local/sbin/ram-fan-virtual-temp
sudo systemctl daemon-reload
```

停止服务不会清除最后一次写入的 `Virtual_TEMP` 值。这个值通常保持到 NCT 复位或系统重启。

## 发布包文件

```text
B850AIGA-RAM-FAN-PatchFix_Linux.tar.gz
├── ram-fan-virtual-temp          # Linux x86_64 成品二进制
├── ram-fan-virtual-temp.service  # systemd 服务模板
├── README.md                     # 用户说明
├── INSTALL.md                    # 安装、更新、验证和卸载
└── LICENSE                       # MIT License
```

## 源码和记录

源码位于 `patch/linux/`。硬件实验、逆向资料和限制记录在 `LOG.md` 与 `WORKFLOW.md` 中。

## 许可证

本项目使用 MIT License。
