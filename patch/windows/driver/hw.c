/* hw.c — 硬件识别与只读访问（阶段 1）
 *
 * 实现（全部来自 LOG.md 已确认事实，不重新调查）：
 *   - RamFanFindSmbusBase：扫描 PCI 配置空间定位 AMD FCH SMBus 控制器
 *     （VEN_1022&DEV_790B），从其 BAR0 读取 SMBus I/O 基址；PCI 路径
 *     不可用时回退到实机已确认的 ACPI 固定基址 0xb00（PNP0C02\700 声明
 *     0xb00-0xb0f）。WORKFLOW §3.2 要求以 PCI/ACPI 资源为准，不能无条件
 *     硬编码，此处回退值有实机证据支持并在 README 记录。
 *   - RamFanProbeNctChipId：NCT 标准 SIO（0x2e/0x2f）解锁后读 0x20/0x21，
 *     实测 0xd8 0x02（NCT6796D-S）。读后写 0xaa 锁定，不留配置模式。
 *   - RamFanSmbusReadWord：模拟固件 HST word-read 序列，带 100ms 超时。
 *   - RamFanCelsiusFromRaw：(raw << 3) >> 5 再 ×25/100。
 *
 * 阶段 1 禁止写 NCT 自定义端口（0x295/0x296 页 0x0c/reg 0x36 在阶段 2）。
 */
#include "ramfan.h"
/* HalGetBusDataByOffset 由 ntddk.h 声明（ramfan.h 已包含），无需 hal.h */

#define PCI_VENDOR_AMD         0x1022
#define PCI_DEVICE_FCH_SMBUS   0x790B
#define PCI_CFG_VENDOR_OFF     0x00
#define PCI_CFG_BAR0_OFF       0x10

/* ACPI PNP0C02\700 已声明 0xb00-0xb0f（实机确认），PCI BAR 不可用时的回退 */
#define RAMFAN_SMBUS_FALLBACK  0xb00

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

    /* PCI 路径不可用：回退到 ACPI 已确认基址（见文件头注释） */
    *baseOut = RAMFAN_SMBUS_FALLBACK;
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
NTSTATUS
RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd, USHORT *rawOut)
{
    PUCHAR hst = (PUCHAR)base;
    LARGE_INTEGER start, now, freq;
    LONGLONG timeoutTicks;
    UCHAR st, d0, d1;

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
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(10); /* 10 us */
    }

    /* 状态检查：0x04=无设备/CRC 错误；0x02 是成功标志，不能判失败 */
    st = READ_PORT_UCHAR(hst + HST_STS_OFF);
    if (st & HST_STS_ERR) {
        return STATUS_DEVICE_NOT_CONNECTED;
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
