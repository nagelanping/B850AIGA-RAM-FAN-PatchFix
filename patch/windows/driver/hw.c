/* hw.c — 只读身份探针 + 受控 SMBus 读取（§5.2 第 1/2 步）
 *
 * 2026-09-05 机主批准的非 PnP 受控模型内允许的硬件访问：
 *   - 系统 PnP 枚举（只读注册表）：确认 FCH SMBus VEN_1022&DEV_790B 存在。
 *     Enum\PCI 下不存在裸 VEN_1022&DEV_790B 键，子键是完整 hardware id
 *     （VEN_1022&DEV_790B&SUBSYS_xxx&REV_xx），枚举该键做前缀匹配；
 *   - 标准 SIO 0x2e/0x2f（白名单）：解锁→读 chip id（0x20/0x21）→锁定；
 *   - SMBus 事务寄存器（基址 0xb00，偏移 0x00..0x06）：§5.2 第 2 步批准，
 *     仅用于读取 SPD/DIMM 数据；必须由驱动内完整序列加锁执行。
 * 本文件不写 NCT 自定义端口 0x295/0x296，不写 NCT 目标寄存器（page 0x0c）。
 */
#include "ramfan.h"

#define RAMFAN_PCI_ENUM_PATH L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\PCI"

static BOOLEAN g_SmbusRecoveryFailed;

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

/* ---- SMBus HST word read（§5.2 第 2 步：受控读取 DIMM） ----
 *
 * 仅允许 base == RAMFAN_SMBUS_RESOURCE_START（0xb00）。调用方必须：
 *   - 已完成身份门禁（ControllerFound && chip id 匹配）；
 *   - 在整个多槽读取序列外加锁，避免并发事务。
 * hstStsOut 回传事务后的原始 HST_STS 供调用方分类（0x02 可为成功标志，
 * 0x04 可能是空槽 NACK 或 CRC/总线异常，不能单独当作固定语义）。
 * 超时：100ms 内 BUSY 未清除 → 有限清理（清状态 + 最多 1ms 观察 BUSY
 * 是否自行解除），不强制复位共享控制器。
 */
NTSTATUS
RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd,
                    USHORT *rawOut, UCHAR *hstStsOut)
{
    PUCHAR hst;
    LARGE_INTEGER start, now, freq;
    LONGLONG timeoutTicks;
    UCHAR st, d0, d1;
    ULONG recovery;

    if (rawOut == NULL || hstStsOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *rawOut = 0;
    *hstStsOut = 0;

    if (base != RAMFAN_SMBUS_RESOURCE_START) {
        return STATUS_ACCESS_DENIED;  /* 白名单：只允许固定目标基址 */
    }
    /* g_SmbusRecoveryFailed：一次 BUSY 恢复失败后本驱动生命周期内拒绝所有后续事务。
     * 平台无法热卸载驱动（sc stop 1052，见 LOG），该 sticky 状态只随驱动重载清除；
     * 服务端输出会把此时各槽记为 TIMEOUT+HstSts=0，与真实单槽超时（HstSts 含 BUSY）
     * 可区分。此标志依赖串行队列保证单线程访问；若未来引入第二个队列/设备需加锁。 */
    if (g_SmbusRecoveryFailed) {
        return STATUS_DEVICE_BUSY;
    }
    hst = (PUCHAR)base;

    /* 清状态 */
    WRITE_PORT_UCHAR(hst + HST_STS_OFF, 0xff);

    /* 从地址（读格式：addr7 << 1 | 1；0x53 -> 0xa7） */
    WRITE_PORT_UCHAR(hst + HST_ADD_OFF, (UCHAR)((addr7 << 1) | 1));

    /* 命令 */
    WRITE_PORT_UCHAR(hst + HST_CMD_OFF, cmd);

    /* 启动 word read */
    WRITE_PORT_UCHAR(hst + HST_CNT_OFF, HST_CNT_WORD_READ);

    /* 轮询 BUSY 清除 */
    start = KeQueryPerformanceCounter(&freq);
    timeoutTicks = freq.QuadPart * RAMFAN_SMBUS_TIMEOUT_MS / 1000;

    for (;;) {
        st = READ_PORT_UCHAR(hst + HST_STS_OFF);
        if (!(st & HST_STS_BUSY)) {
            break;
        }
        now = KeQueryPerformanceCounter(NULL);
        if (now.QuadPart - start.QuadPart > timeoutTicks) {
            /* 有限清理：清状态并确认 BUSY 是否自行解除，不强制复位共享控制器 */
            WRITE_PORT_UCHAR(hst + HST_STS_OFF, 0xff);
            for (recovery = 0; recovery < 100; recovery++) {
                st = READ_PORT_UCHAR(hst + HST_STS_OFF);
                if (!(st & HST_STS_BUSY)) {
                    *hstStsOut = st;
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
    *hstStsOut = st;
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
