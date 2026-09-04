#!/usr/bin/env python3
# 深度只读探测: bank 全页 0x00-0xFF + code=0 全局页 + spd5118 对照
import os, sys, glob
fd = os.open('/dev/port', os.O_RDWR)
def outb(v, p): os.pwrite(fd, bytes([v & 0xFF]), p)
def inb(p): return os.pread(fd, 1, p)[0]
IDX, DAT = 0x295, 0x296
def set_channel(code):
    outb(0x4E, IDX)
    v = inb(DAT)
    outb((v & 0xF0) | (code & 0x0F), DAT)
def rd(reg, code):
    set_channel(code)
    outb(reg, IDX)
    return inb(DAT)
codes = {0: "全局/页0", 1: "块2", 2: "块1", 3: "块3", 8: "块4", 9: "块5(内存?)", 0x0b: "块6"}
for code, nm in codes.items():
    print("==== code 0x%02x (%s) ====" % (code, nm))
    for i in range(0, 0x100, 16):
        row = [rd(r, code) for r in range(i, i+16)]
        if row == [0xff]*16 and i > 0x70:
            if i == 0x80: print("   ... (0x80-0xFF 全 ff)")
            continue
        print("   %02x: %s" % (i, ' '.join('%02x' % x for x in row)))
print("\n== Linux 侧内存温度(spd5118) 现读 ==")
for h in glob.glob('/sys/class/hwmon/hwmon*/name'):
    n = open(h).read().strip()
    if n == 'spd5118':
        d = h.rsplit('/', 1)[0]
        print("  %s: %s m°C" % (d, open(d + '/temp1_input').read().strip()))
