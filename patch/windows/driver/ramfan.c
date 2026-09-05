/* ramfan.c — B850AIGA RAM-FAN PnP 资源识别骨架
 *
 * 当前阶段只把 translated CmResourceTypePort 登记到 PnP 设备上下文：
 *   - SMBus 目标范围：0x0b00-0x0b0f
 *   - NCT 目标范围：0x0290-0x029f
 *   - 标准 SIO 资源范围：0x0200-0x023f
 * 不执行任何端口 I/O，不恢复 SMBus/NCT 事务，不解除 FEED_ONCE=4。
 * 控制设备仅保留 IOCTL 兼容入口；旧 hw.c 不再编译或调用。
 */
#include "ramfan.h"

/* 控制设备上下文；它不拥有 PnP translated resources。 */
typedef struct _RAMFAN_DEVICE_EXTENSION {
    WDFQUEUE Queue;
} RAMFAN_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_DEVICE_EXTENSION, RamFanGetDeviceContext);

/* 每个 PNP0C02 过滤设备独立保存其实际 translated 资源命中情况。 */
typedef struct _RAMFAN_PNP_CONTEXT {
    BOOLEAN SmbusPresent;
    BOOLEAN NctPresent;
    BOOLEAN StandardSioPresent;
    UCHAR Reserved;
    ULONG SmbusStart;
    ULONG SmbusLength;
    ULONG NctStart;
    ULONG NctLength;
    ULONG StandardSioStart;
    ULONG StandardSioLength;
    UCHAR Role;
} RAMFAN_PNP_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_PNP_CONTEXT, RamFanGetPnpContext);

typedef struct _RAMFAN_GLOBAL_CONTEXT {
    WDFWAITLOCK Lock;
    WDFDEVICE SmbusDevice;
    WDFDEVICE NctDevice;
    BOOLEAN SmbusReady;
    BOOLEAN NctReady;
    BOOLEAN SmbusConflict;
    BOOLEAN NctConflict;
} RAMFAN_GLOBAL_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_GLOBAL_CONTEXT, RamFanGetGlobalContext);

#define RAMFAN_ROLE_NONE  0
#define RAMFAN_ROLE_SMBUS 1
#define RAMFAN_ROLE_NCT   2

static BOOLEAN
RamFanResourceContains(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
                       ULONG TargetStart,
                       ULONG TargetLength)
{
    ULONGLONG start;
    ULONGLONG targetEnd;

    if (Descriptor->Type != CmResourceTypePort || Descriptor->u.Port.Length == 0) {
        return FALSE;
    }

    if (Descriptor->u.Port.Start.QuadPart < 0) {
        return FALSE;
    }

    start = (ULONGLONG)Descriptor->u.Port.Start.QuadPart;
    targetEnd = (ULONGLONG)TargetStart + TargetLength - 1;
    if (targetEnd < TargetStart || start > MAXULONG ||
        start > MAXULONGLONG - Descriptor->u.Port.Length) {
        return FALSE;
    }

    return start <= TargetStart &&
           start + Descriptor->u.Port.Length > targetEnd;
}

static VOID
RamFanRememberPort(BOOLEAN *Present,
                   ULONG *Start,
                   ULONG *Length,
                   ULONG TargetStart,
                   ULONG TargetLength)
{
    if (!*Present) {
        *Present = TRUE;
        *Start = TargetStart;
        *Length = TargetLength;
    }
}

static VOID
RamFanResetPnpContext(RAMFAN_PNP_CONTEXT *Context)
{
    RtlZeroMemory(Context, sizeof(*Context));
}

static VOID
RamFanRegisterResources(WDFDEVICE Device, RAMFAN_PNP_CONTEXT *Context)
{
    RAMFAN_GLOBAL_CONTEXT *global;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(Device));
    WdfWaitLockAcquire(global->Lock, NULL);

    if (Context->Role == RAMFAN_ROLE_SMBUS) {
        if (global->SmbusReady && global->SmbusDevice != Device) {
            global->SmbusConflict = TRUE;
        } else {
            global->SmbusDevice = Device;
            global->SmbusReady = TRUE;
        }
    } else if (Context->Role == RAMFAN_ROLE_NCT) {
        if (global->NctReady && global->NctDevice != Device) {
            global->NctConflict = TRUE;
        } else {
            global->NctDevice = Device;
            global->NctReady = TRUE;
        }
    }

    WdfWaitLockRelease(global->Lock);
}

static VOID
RamFanUnregisterResources(WDFDEVICE Device, RAMFAN_PNP_CONTEXT *Context)
{
    RAMFAN_GLOBAL_CONTEXT *global;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(Device));
    WdfWaitLockAcquire(global->Lock, NULL);

    if (Context->Role == RAMFAN_ROLE_SMBUS &&
        global->SmbusDevice == Device) {
        global->SmbusDevice = NULL;
        global->SmbusReady = FALSE;
        global->SmbusConflict = FALSE;
    } else if (Context->Role == RAMFAN_ROLE_NCT &&
               global->NctDevice == Device) {
        global->NctDevice = NULL;
        global->NctReady = FALSE;
        global->NctConflict = FALSE;
    }

    WdfWaitLockRelease(global->Lock);
}

/* ---- DriverEntry（PnP upper-filter + 兼容控制设备） ---- */
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDRIVER driver;
    RAMFAN_GLOBAL_CONTEXT *global;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, RamFanEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attrs, RAMFAN_GLOBAL_CONTEXT);
    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attrs,
                             &config,
                             &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    global = RamFanGetGlobalContext(driver);
    RtlZeroMemory(global, sizeof(*global));
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = driver;
    status = WdfWaitLockCreate(&attrs, &global->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 控制设备不承载端口资源；其 IOCTL 仍在安全阻断状态。 */
    return RamFanCreateDevice(driver);
}

/* ---- PnP 设备与 translated port resource 识别 ---- */
NTSTATUS
RamFanEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = RamFanEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = RamFanEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, RAMFAN_PNP_CONTEXT);
    return WdfDeviceCreate(&DeviceInit, &attrs, &device);
}

NTSTATUS
RamFanEvtPrepareHardware(WDFDEVICE Device,
                         WDFCMRESLIST ResourcesRaw,
                         WDFCMRESLIST ResourcesTranslated)
{
    RAMFAN_PNP_CONTEXT *context;
    ULONG descriptorIndex;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    context = RamFanGetPnpContext(Device);
    RamFanUnregisterResources(Device, context);
    RamFanResetPnpContext(context);
    if (ResourcesTranslated == NULL) {
        return STATUS_SUCCESS;
    }

    for (descriptorIndex = 0;
         descriptorIndex < WdfCmResourceListGetCount(ResourcesTranslated);
         descriptorIndex++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;

        descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated,
                                                    descriptorIndex);
        if (descriptor == NULL || descriptor->Type != CmResourceTypePort) {
            continue;
        }

        if (RamFanResourceContains(descriptor,
                                   RAMFAN_SMBUS_RESOURCE_START,
                                   RAMFAN_SMBUS_RESOURCE_LENGTH)) {
            RamFanRememberPort(&context->SmbusPresent,
                               &context->SmbusStart, &context->SmbusLength,
                               RAMFAN_SMBUS_RESOURCE_START,
                               RAMFAN_SMBUS_RESOURCE_LENGTH);
        }
        if (RamFanResourceContains(descriptor,
                                   RAMFAN_NCT_RESOURCE_START,
                                   RAMFAN_NCT_RESOURCE_LENGTH)) {
            RamFanRememberPort(&context->NctPresent,
                               &context->NctStart, &context->NctLength,
                               RAMFAN_NCT_RESOURCE_START,
                               RAMFAN_NCT_RESOURCE_LENGTH);
        }
        if (RamFanResourceContains(descriptor,
                                   RAMFAN_STANDARD_SIO_RESOURCE_START,
                                   RAMFAN_STANDARD_SIO_RESOURCE_LENGTH)) {
            RamFanRememberPort(&context->StandardSioPresent,
                               &context->StandardSioStart,
                               &context->StandardSioLength,
                               RAMFAN_STANDARD_SIO_RESOURCE_START,
                               RAMFAN_STANDARD_SIO_RESOURCE_LENGTH);
        }
    }

    if (context->SmbusPresent && !context->NctPresent &&
        !context->StandardSioPresent) {
        context->Role = RAMFAN_ROLE_SMBUS;
    } else if (context->NctPresent && context->StandardSioPresent &&
               !context->SmbusPresent) {
        context->Role = RAMFAN_ROLE_NCT;
    }
    RamFanRegisterResources(Device, context);
    return STATUS_SUCCESS;
}

NTSTATUS
RamFanEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    RAMFAN_PNP_CONTEXT *context;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    context = RamFanGetPnpContext(Device);
    RamFanUnregisterResources(Device, context);
    RamFanResetPnpContext(context);
    return STATUS_SUCCESS;
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
     * 当前为 PnP 资源识别骨架；FEED_ONCE 尚未接入资源上下文。
     * translated resources 只做登记，旧 hw.c 访问路径已断开；绝不在此阶段访问端口或写 NCT。
     * 资源模型和后续硬件闭环未完成时，FEED_ONCE 必须在任何硬件访问前失败。
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
    case IOCTL_RAMFAN_QUERY_HW:
    case IOCTL_RAMFAN_READ_DIMM_TEMP:
        /* 旧 hw.c 的 PCI/SIO/SMBus 访问路径已从本驱动断开。 */
        status = STATUS_DEVICE_NOT_READY;
        break;

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
/* ---- 文件/设备清理 ---- */
VOID
RamFanEvtFileClose(WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
}
