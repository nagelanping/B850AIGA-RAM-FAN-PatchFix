/* hw.c — 只读身份探针（§5.2 第 1 步：身份门禁）
 *
 * 2026-09-05 机主批准的非 PnP 受控模型内允许的硬件访问，仅限：
 *   - 系统 PnP 枚举（只读注册表）：确认 FCH SMBus VEN_1022&DEV_790B 存在。
 *     实机 2026-09-05 验证：HalGetBusDataByOffset 在此平台读不到 PCI 配置
 *     空间（x64 legacy CF8/CFC 路径不可用，790B 实为 bus0/dev20/func0），
 *     故改用 pci.sys 已权威枚举的 Enum\PCI 键作为设备存在性证据；
 *   - 标准 SIO 0x2e/0x2f（白名单）：解锁→读 chip id（0x20/0x21）→锁定，
 *     仅用于身份探针，不在驱动内做其他寄存器操作。
 * 本文件不访问 SMBus 事务寄存器（0xb00 偏移 0x00..0x06），不写 NCT
 * 自定义端口 0x295/0x296。SMBus/写回路径待后续步骤批准后再引入。
 */
#include "ramfan.h"

#define RAMFAN_PCI_ENUM_790B_PATH \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\PCI\\VEN_1022&DEV_790B"

/* ---- 确认 FCH SMBus 控制器存在（身份依据；不访问其 I/O 寄存器） ---- */
NTSTATUS
RamFanProbeFchSmbusController(BOOLEAN *foundOut, USHORT *baseOut)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path;
    HANDLE key = NULL;
    NTSTATUS status;

    *foundOut = FALSE;
    if (baseOut != NULL) {
        *baseOut = 0;
    }

    /* pci.sys 已枚举该硬件 ID 才存在此键；只读系统信息，无端口访问。 */
    RtlInitUnicodeString(&path, RAMFAN_PCI_ENUM_790B_PATH);
    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ, &oa);
    if (NT_SUCCESS(status)) {
        *foundOut = TRUE;
        if (baseOut != NULL) {
            /* 固定目标基址是 ACPI/历史证据值，不是从 PCI BAR 探测所得 */
            *baseOut = RAMFAN_SMBUS_RESOURCE_START;
        }
        ZwClose(key);
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
