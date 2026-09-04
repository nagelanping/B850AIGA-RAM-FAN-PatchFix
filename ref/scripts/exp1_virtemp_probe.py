#!/usr/bin/env python3
# 实验1: Virtual_TEMP 值寄存器定位 (OS 侧写测, 可逆)
# 前置: sudo modprobe nct6775; 当前 BIOS 内存风扇源 = 内存温度(0x0a) 失效基线
# 方法: 基于 SkSmartFanProtocol 0x183c 反汇编(读SMBus温度→写页0x0C 0x2e-0x3b),
#       遍历候选 (页, 寄存器) 写 30°C vs 40°C, 观察 hwmon9 pwm5/fan5 是否响应
# 注意: 风扇响应有延迟, 每次写后等待 5s 稳定
# 安全: 写前记录原值, 写后立即恢复; 重启 BIOS 亦可恢复
import os, sys, time, glob

DEV = '/dev/port'
try:
    fd = os.open(DEV, os.O_RDWR)
except OSError as e:
    print("[!] 需要 root: sudo python3 exp1_virtemp_probe.py")
    sys.exit(1)

def outb(v, p): os.pwrite(fd, bytes([v & 0xFF]), p)
def inb(p): return os.pread(fd, 1, p)[0]

IDX, DAT = 0x295, 0x296

def set_channel(code):
    """仿固件: 0x4E index, 低4位=页码 (SkSmartFanCtrlPei 序列)"""
    outb(0x4E, IDX)
    v = inb(DAT)
    outb((v & 0xF0) | (code & 0x0F), DAT)

def rd(reg, code):
    set_channel(code); outb(reg, IDX); return inb(DAT)

def wr(reg, val, code):
    set_channel(code); outb(reg, IDX); outb(val & 0xFF, DAT)

def hwmon_dir(name):
    for h in glob.glob('/sys/class/hwmon/hwmon*/name'):
        if open(h).read().strip() == name:
            return h.rsplit('/', 1)[0]
    return None

def read_sys(path):
    try: return int(open(path).read().strip())
    except Exception: return None

d = hwmon_dir('nct6799')
if not d:
    print("[!] nct6775 未加载。请先: sudo modprobe nct6775")
    sys.exit(1)
print("hwmon dir:", d)

src = rd(0x00, 0x09)
print("内存风扇页0x09 reg00(源) = 0x%02x (期望 0x0a=Virtual_TEMP)" % src)
if src != 0x0a:
    print("[!] 当前源不是 Virtual_TEMP! 请先在 BIOS 把内存风扇数据源设为 内存温度")

bf = read_sys(d + '/fan5_input'); bp = read_sys(d + '/pwm5')
print("基线 fan5=%s rpm pwm5=%s\n" % (bf, bp))

# 页0x0C 优先(0x183c 写回目标页), 其余页对照
cand_banks = [0x0C, 0x09, 0x0B, 0x08, 0x01, 0x02, 0x03]
cand_regs = [0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3b]

print("== 页0x0C 全览(0x00-0x6F, 写测前参考) ==")
for i in range(0, 0x70, 16):
    row = [rd(r, 0x0C) for r in range(i, i + 16)]
    print("   %02x: %s" % (i, ' '.join('%02x' % x for x in row)))
print()

results = []
for bk in cand_banks:
    for rg in cand_regs:
        orig = rd(rg, bk)
        # 写低温 30°C 与 40°C 对比 (NCT 温度寄存器 = °C×1; 曲线陡, 10°C 差应明显)
        wr(rg, 0x1e, bk); time.sleep(5.0)   # 30°C, 等转速稳定
        p1 = read_sys(d + '/pwm5'); f1 = read_sys(d + '/fan5_input')
        wr(rg, 0x28, bk); time.sleep(5.0)   # 40°C
        p2 = read_sys(d + '/pwm5'); f2 = read_sys(d + '/fan5_input')
        wr(rg, orig, bk)  # 恢复原值
        time.sleep(2.0)
        hit = (p1 is not None and p2 is not None and abs(p2 - p1) >= 3)
        mark = "  <<< 响应" if hit else ""
        print("页0x%02x reg0x%02x orig=0x%02x -> 30°C: pwm=%s fan=%s | 40°C: pwm=%s fan=%s%s"
              % (bk, rg, orig, p1, f1, p2, f2, mark))
        if hit:
            results.append((bk, rg, orig, p1, f1, p2, f2))

print("\n===== 响应寄存器汇总 =====")
if not results:
    print("无响应。可能页/寄存器组合不同, 或需按 0x183c 的 (v&0xfc)|0x0c 序列写入")
else:
    for bk, rg, orig, p1, f1, p2, f2 in results:
        print("页0x%02x reg0x%02x (原0x%02x): 30°C -> pwm=%s/%srpm; 40°C -> pwm=%s/%srpm"
              % (bk, rg, orig, p1, f1, p2, f2))
print("\n[完成] 所有候选已恢复原值")
