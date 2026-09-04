#!/usr/bin/env python3
# 内存风扇通道观察: 30s 内随负载变化, 读 NCT 输出/温度读数 + OS 侧温度对照
# 用法: sudo python3 fanwatch.py   (另开终端制造 CPU 负载)
import os, sys, time, glob
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
def os_temp(name):
    for h in glob.glob('/sys/class/hwmon/hwmon*/name'):
        if open(h).read().strip() == name:
            d = h.rsplit('/', 1)[0]
            try:
                return int(open(d + '/temp1_input').read().strip()) / 1000.0
            except Exception:
                return None
    return None
MEM, CPU = 9, 0x02  # 内存风扇通道选择码; 块1(CPU)通道选择码
print("time   mem:out 0x09  t[11..14]      cpuCpuTemp  osCpu  osDimm")
t0 = time.time()
while time.time() - t0 < 30:
    o = rd(0x09, MEM)
    t = [rd(r, MEM) for r in range(0x11, 0x15)]
    cpu = os_temp('k10temp')
    dimm = []
    for h in glob.glob('/sys/class/hwmon/hwmon*/name'):
        if open(h).read().strip() == 'spd5118':
            d = h.rsplit('/', 1)[0]
            try: dimm.append(int(open(d + '/temp1_input').read().strip()) / 1000.0)
            except Exception: pass
    print("%4.1f | %02x    %02x    %-16s %.1f   %s" % (time.time()-t0, o, rd(0x09,CPU), ' '.join('%02x'%x for x in t), cpu if cpu else -1, dimm))
    time.sleep(1)
print("[完成] 观察结束。请提供输出与负载情况。")
