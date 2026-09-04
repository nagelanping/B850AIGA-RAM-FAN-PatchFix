#!/usr/bin/env python3
# SIO 寄存器只读探测 — NCT6796D @ 0x295/0x296 (MS-iCraft B850 AIGA)
# 用法: sudo python3 sio_probe.py
# 安全: 仅读端口, 不修改任何风扇配置。访问序列仿自固件 SkSmartFanCtrlPei。
import os, sys

DEV = '/dev/port'
try:
    fd = os.open(DEV, os.O_RDWR)
except OSError as e:
    print("[!] 无法打开 /dev/port:", e, "需要 root: sudo python3 sio_probe.py")
    sys.exit(1)

def outb(v, p):
    os.pwrite(fd, bytes([v & 0xFF]), p)
def inb(p):
    return os.pread(fd, 1, p)[0]

IDX, DAT = 0x295, 0x296

def set_channel(code):
    """仿固件 0x9c031b6 的访问序列: 0x4E key + 通道码写入数据口低4位"""
    outb(0x4E, IDX)
    v = inb(DAT)
    outb((v & 0xF0) | (code & 0x0F), DAT)

def rd(reg, code):
    set_channel(code)
    outb(reg, IDX)
    return inb(DAT)

codes = {1: 0x02, 2: 0x01, 3: 0x03, 4: 0x08, 5: 0x09, 6: 0x0B}
names = {1: "块1(UI:CPU?)", 2: "块2(UI:Sys1?)", 3: "块3(UI:Sys2?)",
         4: "块4(UI:Sys3?)", 5: "块5(UI:内存?)", 6: "块6(UI:水泵?)"}

print("== 芯片 ID (通道码0): 0x20/0x21 = 0x%02x 0x%02x (NCT6796D 预期 0xD6 0x80/0x90)" % (rd(0x20, 0), rd(0x21, 0)))
print("== 试探标准解锁(0x87,0x87)后读 ID: 0x20/0x21 = 0x%02x 0x%02x" % (inb(0x80), inb(0x81)))
set_channel(0)

# 标准 Nuvoton 解锁序列(0x87,0x87 -> index)后直接读全局寄存器
outb(0x87, IDX); outb(0x87, IDX)
def rdg(reg):
    outb(reg, IDX)
    return inb(DAT)
print("== 标准解锁后 chip id: 0x20/0x21 = 0x%02x 0x%02x (NCT6796D 预期 0xD6 0x80/0x90)" % (rdg(0x20), rdg(0x21)))
for blk, code in codes.items():
    print()
    print("== %s  (选择码 0x%02x)" % (names[blk], code))
    for i in range(0, 0x80, 16):
        row = [rd(r, code) for r in range(i, i+16)]
        print("   %02x: %s" % (i, ' '.join('%02x' % x for x in row)))

print()
print("== 关键寄存器汇总 ===")
print("reg  00(源?) 03 04 05 08 09 21 22 23 24 25 27 28 29 2A 2B 2C 35 61 62 63 64 65 66")
for blk, code in codes.items():
    k = [rd(r, code) for r in [0x00,0x03,0x04,0x05,0x08,0x09,0x21,0x22,0x23,0x24,0x25,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x35,0x61,0x62,0x63,0x64,0x65,0x66]]
    print("块%d   %s" % (blk, ' '.join('%02x' % x for x in k)))

print("\n[完成] 只读探测结束。请把完整输出贴给分析者。")