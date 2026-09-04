#!/usr/bin/env python3
# 实验2: SMBUSMASTER 硬件探测 (子方案C可行性: NCT SMBus 是否连 DIMM)
# 方法: 标准 SIO (0x2E/0x2F) 解锁后枚举逻辑设备(LDN)基址, 找 SMBus 设备;
#       尝试用探测到的 SMBus 基址读 DIMM 温度传感器 (TSE2004/SPD5118, 从地址 0x53<<1)
# 安全: 只读为主; SMBus 读尝试失败即停
import os, sys, time

DEV = '/dev/port'
try:
    fd = os.open(DEV, os.O_RDWR)
except OSError as e:
    print("[!] 需要 root: sudo python3 exp2_smbus_probe.py")
    sys.exit(1)

def outb(v, p): os.pwrite(fd, bytes([v & 0xFF]), p)
def inb(p): return os.pread(fd, 1, p)[0]

IDX, DAT = 0x2E, 0x2F

def sio_unlock():
    outb(0x87, IDX); outb(0x87, IDX)

def sio_rd(reg):
    outb(reg, IDX); return inb(DAT)

def sio_wr(reg, v):
    outb(reg, IDX); outb(v & 0xFF, DAT)

def sio_ldn(n):
    sio_wr(0x07, n)  # LDN 选择

sio_unlock()
cid = (sio_rd(0x20), sio_rd(0x21))
print("SIO chip id: 0x%02x 0x%02x" % cid)

print("\n== 逻辑设备(LDN)基址枚举 ==")
for ldn in range(0x00, 0x18):
    sio_ldn(ldn)
    act = sio_rd(0x30) & 0x01
    io0 = sio_rd(0x60) | (sio_rd(0x61) << 8)
    io1 = sio_rd(0x62) | (sio_rd(0x63) << 8)
    print("LDN 0x%02x: active=%d io0=0x%04x io1=0x%04x" % (ldn, act, io0, io1))

print("\n== SMBus 读尝试 (南桥 0xb00 为固件所用; NCT 自身 SMBus 需手册确认) ==")
# 固件读内存温度的序列 (SkSmartFanProtocol 0x183c): 基址 0xb00, 从地址 0xa6/0xa7, 命令 0x31
def smbus_read_hst(base, slave, cmd, nbytes=2):
    """模拟固件: 清状态→从地址→命令→控制(0x4c=word read)→读数据"""
    try:
        outb(0xff, base + 0x00)          # HST_STS 清
        outb(slave | 0x01, base + 0x04)  # XMIT_SLVA (读位)
        outb(cmd, base + 0x03)           # HST_CMD
        outb(0x4c, base + 0x02)          # HST_CNT: word read + start
        t0 = time.time()
        while time.time() - t0 < 0.1:
            st = inb(base + 0x00)
            if not (st & 0x01):          # BUSY 清除
                break
        else:
            return None, "timeout"
        st = inb(base + 0x00)
        if st & 0x04:
            return None, "err 0x%02x" % st
        d0 = inb(base + 0x05); d1 = inb(base + 0x06)
        raw = (d0 | (d1 << 8))
        t = (raw << 3 >> 5) * 25 / 100   # 固件换算
        return (d0, d1, raw, t), None
    except Exception as e:
        return None, str(e)

# 南桥 SMBus 0xb00 (固件实际使用) — 验证 OS 下是否可读
r, err = smbus_read_hst(0xb00, 0xa6, 0x31)
print("0xb00 从0x53读cmd0x31:", r if r else ("失败 " + str(err)))

# 尝试 NCT 自身 SMBus 基址 (若 LDN 中有 SMBus)
for ldn in range(0x00, 0x18):
    sio_ldn(ldn)
    io0 = sio_rd(0x60) | (sio_rd(0x61) << 8)
    if io0 and io0 != 0xffff:
        r, err = smbus_read_hst(io0, 0xa6, 0x31)
        print("LDN0x%02x io0=0x%04x SMBus读:", ldn, io0,
              r if r else ("失败 " + str(err)))

print("\n[完成]")
