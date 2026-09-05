/* ramfan.c — B850AIGA RAM-FAN Virtual_TEMP 补丁驱动（阶段 2）
 *
 * 阶段 2 范围（WORKFLOW.md §4 阶段 2）：
 * 阶段 2范围（当前安全阻断）：
 *   - 保留 IOCTL_RAMFAN_FEED_ONCE 接口，但在资源模型解决前立即返回
 *     RAMFAN_FEED_HW_UNAVAILABLE，不访问 SMBus/NCT，不执行写回。
 *   - 服务常驻喂值留到阶段 3；禁止改写 BIOS、曲线或 page 0x09 温度源寄存器。
 *
 * 驱动模型：非 PnP 遗留内核驱动 + KMDF 对象。WdfDriverInitNonPnpDriver
 * 模式下 EvtDriverDeviceAdd 不会被调用，设备在 DriverEntry 中手动创建
 * （WdfControlDeviceInitAllocate + WdfDeviceCreate）。硬件访问顺序由串行队列
 * 保证本驱动内不交错；不能与外部硬件监控程序原子化。
 */
#include "ramfan.h"

/* ---- 设备扩展 ----
 * ponytail: 非 PnP 驱动没有 WDF 资源回调，阶段 1 只在请求时做硬件识别；
 * 阶段 2 若改绑 PnP 设备再迁移到 EvtDevicePrepareHardware。 */
typedef struct _RAMFAN_DEVICE_EXTENSION {
    WDFQUEUE Queue;
} RAMFAN_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_DEVICE_EXTENSION, RamFanGetDeviceContext);

/* ---- 前置声明 ---- */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RamFanEvtIoDeviceControl;
EVT_WDF_FILE_CLOSE RamFanEvtFileClose;
NTSTATUS RamFanCreateDevice(WDFDRIVER Driver);
EVT_WDF_DRIVER_UNLOAD RamFanEvtDriverUnload;
/* hw.c 中实现 */
NTSTATUS RamFanFindSmbusBase(USHORT *baseOut);
NTSTATUS RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo);
NTSTATUS RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd,
                             USHORT *rawOut);
ULONG    RamFanCelsiusFromRaw(USHORT raw);

/* ---- SPD 7-bit 地址轮询顺序（LOG.md 已确认） ---- */
static const UCHAR RamFanSpdAddrs[RAMFAN_SPD_ADDR_COUNT] = {
    RAMFAN_SPD_ADDR_53, RAMFAN_SPD_ADDR_52,
    RAMFAN_SPD_ADDR_51, RAMFAN_SPD_ADDR_50
};

/* ---- DriverEntry（非 PnP：手动创建设备） ---- */
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDRIVER driver;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, NULL);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = RamFanEvtDriverUnload; /* 非 PnP 驱动必需 */
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attrs,
                             &config,
                             &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return RamFanCreateDevice(driver);
}

/* ---- 手动创建控制设备 ---- */
NTSTATUS
RamFanCreateDevice(WDFDRIVER Driver)
{
    PWDFDEVICE_INIT deviceInit;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    RAMFAN_DEVICE_EXTENSION *ext;
    WDF_IO_QUEUE_CONFIG queueConfig;
    NTSTATUS status;
    UNICODE_STRING dosName, devName, sddl;
    WDF_FILEOBJECT_CONFIG fileConfig;

    /* 控制设备：SDDL 限 SYSTEM/管理员；初始化结构由框架管理 */
    RtlInitUnicodeString(&sddl, RAMFAN_DEVICE_SDDL);
    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 缓冲型 IOCTL + 独占（同一时刻仅一个打开句柄，配合串行队列） */
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, TRUE);
    /* 设备名 + 符号链接 */
    RtlInitUnicodeString(&devName, RAMFAN_DEVICE_NAME);
    status = WdfDeviceInitAssignName(deviceInit, &devName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    /* 文件对象回调（顺序：EvtDeviceFileCreate, EvtFileClose, EvtFileCleanup） */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, NULL, RamFanEvtFileClose, NULL);
    WdfDeviceInitSetFileObjectConfig(deviceInit,
                                     &fileConfig,
                                     WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attrs, RAMFAN_DEVICE_EXTENSION);

    status = WdfDeviceCreate(&deviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ext = RamFanGetDeviceContext(device);

    RtlInitUnicodeString(&dosName, RAMFAN_DOS_DEVICE_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &dosName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 默认队列：串行，只分发 IOCTL */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = RamFanEvtIoDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    status = WdfIoQueueCreate(device, &queueConfig, &attrs, &ext->Queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 控制设备必须显式结束初始化，之后才接受 I/O */
    WdfControlFinishInitializing(device);

    return STATUS_SUCCESS;
}

/* ---- 阶段 2：一次完整读取、校验、写回 ---- */
static NTSTATUS
RamFanFeedOnce(RAMFAN_DEVICE_EXTENSION *ext,
                RAMFAN_FEED_ONCE_OUT *out)
{
    /*
     * 当前仍是非 PnP 控制设备：没有 EvtDevicePrepareHardware，也没有
     * translated resource list。PCI BAR/ACPI _CRS 证据不能授权本设备访问
     * 端口，尤其不能授权独立 PNP0C02 的 NCT 端口。
     *
     * 资源模型解决前，FEED_ONCE 必须在任何硬件访问前失败，绝不写 NCT。
     */
    UNREFERENCED_PARAMETER(ext);
    out->Status = RAMFAN_FEED_HW_UNAVAILABLE;
    return STATUS_SUCCESS;
}


/* ---- EvtIoDeviceControl ---- */
VOID
RamFanEvtIoDeviceControl(WDFQUEUE Queue,
                         WDFREQUEST Request,
                         size_t OutputBufferLength,
                         size_t InputBufferLength,
                         ULONG IoControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PVOID outBuffer = NULL;
    size_t outLen = 0;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_RAMFAN_QUERY_HW: {
        RAMFAN_QUERY_HW_OUT out = {0};
        USHORT base = 0;
        UCHAR hi = 0, lo = 0;

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = RamFanFindSmbusBase(&base);
        if (NT_SUCCESS(status)) {
            out.SmbusBase = base;
        } else {
            out.SmbusBase = 0;
        }

        status = RamFanProbeNctChipId(&hi, &lo);
        if (NT_SUCCESS(status)) {
            out.ChipIdHi = hi;
            out.ChipIdLo = lo;
        } else {
            out.ChipIdHi = 0;
            out.ChipIdLo = 0;
        }

        out.HwMatched =
            (out.SmbusBase != 0) &&
            (out.ChipIdHi == NCT_EXPECTED_CHIP_ID_HI) &&
            (out.ChipIdLo == NCT_EXPECTED_CHIP_ID_LO);

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out),
                                                &outBuffer, &outLen);
        if (!NT_SUCCESS(status)) {
            break;
        }
        RtlCopyMemory(outBuffer, &out, sizeof(out));
        WdfRequestSetInformation(Request, sizeof(out));
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_RAMFAN_READ_DIMM_TEMP: {
        RAMFAN_READ_DIMM_OUT out = {0};
        USHORT base = 0;
        UCHAR i;

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        /* 基址每次现查，避免依赖启动顺序 */
        status = RamFanFindSmbusBase(&base);
        if (!NT_SUCCESS(status)) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }

        out.Count = RAMFAN_SPD_ADDR_COUNT;
        for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
            RAMFAN_DIMM_RESULT *slot = &out.Slots[i];
            USHORT raw;
            NTSTATUS rd;

            slot->Address = RamFanSpdAddrs[i];
            rd = RamFanSmbusReadWord(base, slot->Address, SPD_CMD_TEMP, &raw);
            if (NT_SUCCESS(rd)) {
                ULONG c = RamFanCelsiusFromRaw(raw); /* ULONG：校验先于截断 */
                if (c > RAMFAN_TEMP_MAX) { /* ULONG 域校验：0..120 之外判非法 */
                    slot->Status = RAMFAN_DIMM_BAD_DATA;
                } else {
                    slot->Status = RAMFAN_DIMM_OK;
                    slot->Raw = raw;
                    slot->Celsius = (UCHAR)c; /* 已通过 0..120 校验，截断安全 */
                    out.AnySuccess = TRUE;
                    if (c > out.MaxCelsius) {
                        out.MaxCelsius = (UCHAR)c;
                    }
                }
            } else if (rd == STATUS_DEVICE_NOT_CONNECTED) {
                slot->Status = RAMFAN_DIMM_NACK; /* 空槽，不算失败 */
            } else if (rd == STATUS_IO_TIMEOUT) {
                slot->Status = RAMFAN_DIMM_TIMEOUT;
            } else {
                slot->Status = RAMFAN_DIMM_BUS_ERR;
            }
        }

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out),
                                                &outBuffer, &outLen);
        if (!NT_SUCCESS(status)) {
            break;
        }
        RtlCopyMemory(outBuffer, &out, sizeof(out));
        WdfRequestSetInformation(Request, sizeof(out));
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_RAMFAN_FEED_ONCE: {
        RAMFAN_FEED_ONCE_OUT out = {0};
        WDFDEVICE device;
        RAMFAN_DEVICE_EXTENSION *ext;

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        device = WdfIoQueueGetDevice(Queue);
        ext = RamFanGetDeviceContext(device);
        status = RamFanFeedOnce(ext, &out);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out),
                                                 &outBuffer, &outLen);
        if (!NT_SUCCESS(status)) {
            break;
        }
        RtlCopyMemory(outBuffer, &out, sizeof(out));
        WdfRequestSetInformation(Request, sizeof(out));
        status = STATUS_SUCCESS;
        break;
    }

    default:
        break;
    }

    if (status == STATUS_INVALID_DEVICE_REQUEST ||
        status == STATUS_BUFFER_TOO_SMALL) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    /* 成功路径已 SetInformation；此处仅完成请求 */
    WdfRequestComplete(Request, status);
}
/* ---- 驱动卸载（非 PnP 驱动必需回调） ---- */
VOID
RamFanEvtDriverUnload(WDFDRIVER Driver)
{
    /* WDF 自动清理设备对象/队列；此处仅作钩子，无自定义资源 */
    UNREFERENCED_PARAMETER(Driver);
}

/* ---- 文件/设备清理 ---- */
VOID
RamFanEvtFileClose(WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
}
