/* hw.c — 只读身份探针（§5.2 第 1 步：身份门禁）
 *
 * 2026-09-05 机主批准的非 PnP 受控模型内允许的硬件访问，仅限：
 *   - PCI 配置空间读取（只读）：确认 FCH SMBus VEN_1022&DEV_790B 存在；
 *   - 标准 SIO 0x2e/0x2f（白名单）：解锁→读 chip id（0x20/0x21）→锁定，
 *     仅用于身份探针，不在驱动内做其他寄存器操作。
 * 本文件不访问 SMBus 事务寄存器（0xb00 偏移 0x00..0x06），不写 NCT
 * 自定义端口 0x295/0x296。SMBus/写回路径待后续步骤批准后再引入。
 */
#include "ramfan.h"

#define PCI_VENDOR_AMD       0x1022
#define PCI_DEVICE_FCH_SMBUS 0x790B
#define PCI_CFG_VENDOR_OFF   0x00
#define PCI_CFG_BAR0_OFF     0x10

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

/* ---- 定位 FCH SMBus 控制器（身份依据；不访问其 I/O 寄存器） ---- */
NTSTATUS
RamFanProbeFchSmbusController(BOOLEAN *foundOut, USHORT *baseOut)
{
    ULONG bus, device, func;

    *foundOut = FALSE;
    if (baseOut != NULL) {
        *baseOut = 0;
    }

    for (bus = 0; bus < 16; bus++) {
        for (device = 0; device < 32; device++) {
            for (func = 0; func < 8; func++) {
                ULONG vidDid = 0;
                ULONG bar0 = 0;

                if (!NT_SUCCESS(RamFanReadPciCfg(bus, device, func,
                                                 PCI_CFG_VENDOR_OFF, &vidDid))) {
                    continue;
                }
                if ((vidDid & 0xFFFF) != PCI_VENDOR_AMD) {
                    continue;
                }
                if (((vidDid >> 16) & 0xFFFF) != PCI_DEVICE_FCH_SMBUS) {
                    continue;
                }
                *foundOut = TRUE;

                if (baseOut != NULL &&
                    NT_SUCCESS(RamFanReadPciCfg(bus, device, func,
                                                PCI_CFG_BAR0_OFF, &bar0))) {
                    /* BAR0：bit0=1 表示 I/O 空间；掩码 0xFFFC 取基址。
                       BAR 无效（未映射）时保持 0，不做猜测回退。 */
                    if ((bar0 & 0x1) && (bar0 & 0xFFFC) != 0) {
                        *baseOut = (USHORT)(bar0 & 0xFFFC);
                    }
                }
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_SUCCESS;
}

/* ---- NCT chip id（标准 SIO 0x2e/0x2f，白名单身份探针） ---- */
NTSTATUS
RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo)
{
    PUCHAR idx = (PUCHAR)NCT_STD_IDX;
    PUCHAR dat = (PUCHAR)NCT_STD_DAT;
    UCHAR idHi, idLo;

    if (hi == NULL || lo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *hi = 0xff;
    *lo = 0xff;

    /* Nuvoton 标准解锁序列（exp2_smbus_probe.py 已验证） */
    WRITE_PORT_UCHAR(idx, 0x87);
    WRITE_PORT_UCHAR(idx, 0x87);

    WRITE_PORT_UCHAR(idx, 0x20);
    idHi = READ_PORT_UCHAR(dat);
    WRITE_PORT_UCHAR(idx, 0x21);
    idLo = READ_PORT_UCHAR(dat);

    /* 锁定 SIO，避免遗留配置模式影响其他访问者（成功或失败都执行） */
    WRITE_PORT_UCHAR(idx, 0xaa);

    if (idHi == 0xff && idLo == 0xff) {
        return STATUS_NOT_FOUND;
    }
    *hi = idHi;
    *lo = idLo;
    return STATUS_SUCCESS;
}
