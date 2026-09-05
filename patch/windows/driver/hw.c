/* hw.c — 只读身份探针（§5.2 第 1 步：身份门禁）
 *
 * 2026-09-05 机主批准的非 PnP 受控模型内允许的硬件访问，仅限：
 *   - 系统 PnP 枚举（只读注册表）：确认 FCH SMBus VEN_1022&DEV_790B 存在。
 *     实机 2026-09-05 验证：HalGetBusDataByOffset 在此平台读不到 PCI 配置
 *     空间（x64 legacy CF8/CFC 路径不可用，790B 实为 bus0/dev20/func0）。
 *     Enum\PCI 下不存在裸 VEN_1022&DEV_790B 键，子键是完整 hardware id
 *     （VEN_1022&DEV_790B&SUBSYS_xxx&REV_xx），故枚举该键做前缀匹配；
 *   - 标准 SIO 0x2e/0x2f（白名单）：解锁→读 chip id（0x20/0x21）→锁定，
 *     仅用于身份探针，不在驱动内做其他寄存器操作。
 * 本文件不访问 SMBus 事务寄存器（0xb00 偏移 0x00..0x06），不写 NCT
 * 自定义端口 0x295/0x296。SMBus/写回路径待后续步骤批准后再引入。
 */
#include "ramfan.h"

#define RAMFAN_PCI_ENUM_PATH L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\PCI"

/* ---- 确认 FCH SMBus 控制器存在（身份依据；不访问其 I/O 寄存器） ---- */
NTSTATUS
RamFanProbeFchSmbusController(BOOLEAN *foundOut, USHORT *baseOut)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path;
    UNICODE_STRING prefix = RTL_CONSTANT_STRING(L"VEN_1022&DEV_790B");
    HANDLE key = NULL;
    NTSTATUS status;
    PKEY_FULL_INFORMATION full = NULL;
    PKEY_BASIC_INFORMATION basic = NULL;
    ULONG fullSize = 0;
    ULONG basicSize = 0;
    ULONG index;
    BOOLEAN found = FALSE;

    *foundOut = FALSE;
    if (baseOut != NULL) {
        *baseOut = 0;
    }

    RtlInitUnicodeString(&path, RAMFAN_PCI_ENUM_PATH);
    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ, &oa);
    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    /* 第一次查询拿 KEY_FULL_INFORMATION 所需大小（取子键数量与最大名长） */
    status = ZwQueryKey(key, KeyFullInformation, NULL, 0, &fullSize);
    if (status != STATUS_BUFFER_OVERFLOW && status != STATUS_BUFFER_TOO_SMALL) {
        ZwClose(key);
        return STATUS_SUCCESS;
    }
    fullSize += 256; /* 保守余量 */
    full = (PKEY_FULL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED,
                                                  fullSize, 'fRfl');
    if (full == NULL) {
        ZwClose(key);
        return STATUS_SUCCESS;
    }
    status = ZwQueryKey(key, KeyFullInformation, full, fullSize, &fullSize);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(full, 'fRfl');
        ZwClose(key);
        return STATUS_SUCCESS;
    }

    basicSize = sizeof(KEY_BASIC_INFORMATION) + full->MaxNameLen +
                sizeof(WCHAR);
    basic = (PKEY_BASIC_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED,
                                                    basicSize, 'fRbs');
    if (basic == NULL) {
        ExFreePoolWithTag(full, 'fRfl');
        ZwClose(key);
        return STATUS_SUCCESS;
    }

    for (index = 0; index < full->SubKeys && !found; index++) {
        UNICODE_STRING name;

        status = ZwEnumerateKey(key, index, KeyBasicInformation,
                                basic, basicSize, &basicSize);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        name.Length = (USHORT)basic->NameLength;
        name.MaximumLength = (USHORT)basic->NameLength;
        name.Buffer = basic->Name;
        if (RtlPrefixUnicodeString(&prefix, &name, TRUE)) {
            found = TRUE;
        }
    }

    if (found) {
        *foundOut = TRUE;
        if (baseOut != NULL) {
            /* 固定目标基址是 ACPI/历史证据值，不是从 PCI BAR 探测所得 */
            *baseOut = RAMFAN_SMBUS_RESOURCE_START;
        }
    }

    ExFreePoolWithTag(basic, 'fRbs');
    ExFreePoolWithTag(full, 'fRfl');
    ZwClose(key);
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
