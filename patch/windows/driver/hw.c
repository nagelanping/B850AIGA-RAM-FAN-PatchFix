/* hw.c — 硬件识别与只读访问（阶段 1）
 *
 * 实现（全部来自 LOG.md 已确认事实，不重新调查）：
 *   - RamFanFindSmbusBase：阶段 1 的只读探测辅助；它不能证明当前驱动
 *     拥有 translated resource。阶段 2 写回在资源模型解决前被明确禁用。
 *   - RamFanProbeNctChipId：NCT 标准 SIO（0x2e/0x2f）解锁后读 0x20/0x21，
 *     实测 0xd8 0x02（NCT6796D-S）。读后写 0xaa 锁定，不留配置模式。
 *   - RamFanSmbusReadWord：模拟固件 HST word-read 序列，带 100ms 超时。
 *   - RamFanCelsiusFromRaw：(raw << 3) >> 5 再 ×25/100。
 *
 * 阶段 1 禁止写 NCT 自定义端口（0x295/0x296）。阶段 2 写回仍被资源门禁阻止。
 */
#include "ramfan.h"
/* HalGetBusDataByOffset 由 ntddk.h 声明（ramfan.h 已包含），无需 hal.h */

#define PCI_VENDOR_AMD         0x1022
#define PCI_DEVICE_FCH_SMBUS   0x790B
#define PCI_CFG_VENDOR_OFF     0x00
#define PCI_CFG_BAR0_OFF       0x10

/* 仅用于阶段 1 只读诊断；不是当前驱动的 translated resource。 */
#define RAMFAN_SMBUS_DIAGNOSTIC_BASE  0xb00

/* ---- PCI 配置空间读取（bus 0-15，device 0-31，function 0-7） ---- */
static NTSTATUS
RamFanReadPciCfg(ULONG bus, ULONG device, ULONG func,
                 ULONG offset, ULONG *valueOut)
{
    ULONG slot = (device << 16) | (func << 8);
    ULONG value = 0;
    ULONG n;

    n = HalGetBusDataByOffset(PCIConfiguration, bus, slot,
                              &value, offset, sizeof(value));
    if (n != sizeof(value)) {
        return STATUS_NOT_FOUND;
    }
    *valueOut = value;
    return STATUS_SUCCESS;
}

/* ---- 定位 FCH SMBus 控制器并取 I/O 基址 ---- */
NTSTATUS
RamFanFindSmbusBase(USHORT *baseOut)
{
    ULONG bus, device, func;
    NTSTATUS status;

    *baseOut = 0;

    for (bus = 0; bus < 16; bus++) {
        for (device = 0; device < 32; device++) {
            for (func = 0; func < 8; func++) {
                ULONG vidDid = 0;
                ULONG bar0 = 0;

                status = RamFanReadPciCfg(bus, device, func,
                                          PCI_CFG_VENDOR_OFF, &vidDid);
                if (!NT_SUCCESS(status)) {
                    continue;
                }
                if ((vidDid & 0xFFFF) != PCI_VENDOR_AMD) {
                    continue;
                }
                if (((vidDid >> 16) & 0xFFFF) != PCI_DEVICE_FCH_SMBUS) {
                    continue;
                }

                status = RamFanReadPciCfg(bus, device, func,
                                          PCI_CFG_BAR0_OFF, &bar0);
                if (!NT_SUCCESS(status)) {
                    continue;
                }

                /* BAR0：bit0=1 表示 I/O 空间；掩码 0xFFFC 取基址 */
                if ((bar0 & 0x1) && (bar0 & 0xFFFC) != 0) {
                    *baseOut = (USHORT)(bar0 & 0xFFFC);
                    return STATUS_SUCCESS;
                }
            }
        }
    }

    /* 阶段 1 诊断回退；写回路径不得把它当作资源授权。 */
    *baseOut = RAMFAN_SMBUS_DIAGNOSTIC_BASE;
    return STATUS_SUCCESS;
}

/* ---- NCT chip id（标准 SIO 0x2e/0x2f） ---- */
NTSTATUS
RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo)
{
    PUCHAR idx = (PUCHAR)NCT_STD_IDX;
    PUCHAR dat = (PUCHAR)NCT_STD_DAT;
    UCHAR idHi, idLo;

    /* Nuvoton 标准解锁序列（exp2_smbus_probe.py 已验证） */
    WRITE_PORT_UCHAR(idx, 0x87);
    WRITE_PORT_UCHAR(idx, 0x87);

    WRITE_PORT_UCHAR(idx, 0x20);
    idHi = READ_PORT_UCHAR(dat);
    WRITE_PORT_UCHAR(idx, 0x21);
    idLo = READ_PORT_UCHAR(dat);

    /* 锁定 SIO，避免遗留配置模式影响其他访问者 */
    WRITE_PORT_UCHAR(idx, 0xaa);

    if (idHi == 0xff && idLo == 0xff) {
        return STATUS_NOT_FOUND;
    }

    *hi = idHi;
    *lo = idLo;
    return STATUS_SUCCESS;
}

/* ---- SMBus HST word read（模拟固件序列，100ms 超时） ---- */
static BOOLEAN g_SmbusRecoveryFailed;

NTSTATUS
RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd, USHORT *rawOut)
{
    PUCHAR hst = (PUCHAR)base;
    LARGE_INTEGER start, now, freq;
    LONGLONG timeoutTicks;
    UCHAR st, d0, d1;
    ULONG recovery;
    if (g_SmbusRecoveryFailed) {
        return STATUS_DEVICE_BUSY;
    }

    /* 清状态 */
    WRITE_PORT_UCHAR(hst + HST_STS_OFF, 0xff);

    /* 从地址（读格式：addr7 << 1 | 1；0x53 -> 0xa7） */
    WRITE_PORT_UCHAR(hst + HST_ADD_OFF, (UCHAR)((addr7 << 1) | 1));

    /* 命令 */
    WRITE_PORT_UCHAR(hst + HST_CMD_OFF, cmd);

    /* 启动 word read */
    WRITE_PORT_UCHAR(hst + HST_CNT_OFF, HST_CNT_WORD_READ);

    /* 轮询 BUSY 清除（26100 SDK：KeQueryPerformanceCounter 返回计数器，频率为出参） */
    start = KeQueryPerformanceCounter(&freq);
    timeoutTicks = freq.QuadPart * RAMFAN_SMBUS_TIMEOUT_MS / 1000;

    for (;;) {
        st = READ_PORT_UCHAR(hst + HST_STS_OFF);
        if (!(st & HST_STS_BUSY)) {
            break;
        }
        now = KeQueryPerformanceCounter(NULL);
        if (now.QuadPart - start.QuadPart > timeoutTicks) {
            /* 有限清理：清除状态并确认 BUSY 是否自行解除，不强制复位共享控制器。 */
            WRITE_PORT_UCHAR(hst + HST_STS_OFF, 0xff);
            for (recovery = 0; recovery < 100; recovery++) {
                st = READ_PORT_UCHAR(hst + HST_STS_OFF);
                if (!(st & HST_STS_BUSY)) {
                    return STATUS_IO_TIMEOUT;
                }
                KeStallExecutionProcessor(10); /* 最多再等 1ms */
            }
            g_SmbusRecoveryFailed = TRUE;
            return STATUS_DEVICE_BUSY;
        }
        KeStallExecutionProcessor(10); /* 10 us */
    }

    /* 0x04 可能是空槽 NACK，也可能是 CRC/总线异常；不能冒充空槽。 */
    st = READ_PORT_UCHAR(hst + HST_STS_OFF);
    if (st & HST_STS_ERR) {
        return STATUS_DATA_ERROR;
    }

    d0 = READ_PORT_UCHAR(hst + HST_DAT0_OFF);
    d1 = READ_PORT_UCHAR(hst + HST_DAT1_OFF);
    *rawOut = (USHORT)(d0 | (d1 << 8));
    return STATUS_SUCCESS;
}

/* ---- 温度换算：(raw << 3) >> 5，再 ×25/100 ----
 * 返回 ULONG（不截断），由调用方在 0..120 校验后才转 UCHAR；
 * 避免先截断把 256 等越界值折叠成 0°C 绕过范围检查。 */
ULONG
RamFanCelsiusFromRaw(USHORT raw)
{
    ULONG scaled = ((ULONG)raw << 3) >> 5;
    return (scaled * 25) / 100;
}
