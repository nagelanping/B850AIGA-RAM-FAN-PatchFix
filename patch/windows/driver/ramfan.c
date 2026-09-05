/* ramfan.c — B850AIGA RAM-FAN PnP 资源识别骨架
 *
 * 当前阶段只把 translated CmResourceTypePort 登记到 PnP 设备上下文：
 *   - SMBus 目标范围：0x0b00-0x0b0f
 *   - NCT 目标范围：0x0290-0x029f
 *   - 标准 SIO 资源范围：0x002e-0x002f
 * 不执行任何端口 I/O，不恢复 SMBus/NCT 事务，不解除 FEED_ONCE=4。
 * 控制设备仅保留 IOCTL 兼容入口；旧 hw.c 不再编译或调用。
 */
#include "ramfan.h"
#include <devpkey.h>

static const DEVPROPKEY RamFanDeviceInstanceIdKey = {
    {0x78c34fc8, 0x104a, 0x4aca,
     {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}},
    256
};

/* 控制设备上下文；它不拥有 PnP translated resources。 */
typedef struct _RAMFAN_DEVICE_EXTENSION {
    WDFQUEUE Queue;
} RAMFAN_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_DEVICE_EXTENSION, RamFanGetDeviceContext);

/* 每个 PNP0C02 过滤设备独立保存其实际 translated 资源命中情况。 */
typedef struct _RAMFAN_PNP_CONTEXT {
    RAMFAN_RESOURCE_MAP Resources;
    BOOLEAN RegisteredReference;
} RAMFAN_PNP_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_PNP_CONTEXT, RamFanGetPnpContext);

typedef struct _RAMFAN_GLOBAL_CONTEXT {
    WDFWAITLOCK Lock;
    WDFDEVICE SmbusDevice;
    WDFDEVICE NctDevice;
    ULONG SmbusStart;
    ULONG SmbusLength;
    ULONG NctStart;
    ULONG NctLength;
    ULONG StandardSioStart;
    ULONG StandardSioLength;
    BOOLEAN SmbusReady;
    BOOLEAN NctReady;
    BOOLEAN SmbusConflict;
    BOOLEAN NctConflict;
    ULONG ActiveUsers;
    KEVENT ActiveUsersZero;
} RAMFAN_GLOBAL_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_GLOBAL_CONTEXT, RamFanGetGlobalContext);

typedef struct _RAMFAN_TRANSACTION {
    ULONG SmbusStart;
    ULONG SmbusLength;
    ULONG NctStart;
    ULONG NctLength;
    ULONG StandardSioStart;
    ULONG StandardSioLength;
} RAMFAN_TRANSACTION;

static BOOLEAN
RamFanBeginHardwareTransaction(WDFDEVICE ControlDevice,
                               RAMFAN_TRANSACTION *Transaction)
{
    RAMFAN_GLOBAL_CONTEXT *global;
    BOOLEAN acquired = FALSE;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(ControlDevice));
    RtlZeroMemory(Transaction, sizeof(*Transaction));
    WdfWaitLockAcquire(global->Lock, NULL);
    if (RamFanBeginHardwareTransactionState(global->SmbusReady,
                                            global->SmbusConflict,
                                            global->NctReady,
                                            global->NctConflict,
                                            &global->ActiveUsers)) {
        if (global->ActiveUsers == 1) {
            KeClearEvent(&global->ActiveUsersZero);
        }
        Transaction->SmbusStart = global->SmbusStart;
        Transaction->SmbusLength = global->SmbusLength;
        Transaction->NctStart = global->NctStart;
        Transaction->NctLength = global->NctLength;
        Transaction->StandardSioStart = global->StandardSioStart;
        Transaction->StandardSioLength = global->StandardSioLength;
        acquired = TRUE;
    }
    WdfWaitLockRelease(global->Lock);
    return acquired;
}

static VOID
RamFanEndHardwareTransaction(WDFDEVICE ControlDevice)
{
    RAMFAN_GLOBAL_CONTEXT *global;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(ControlDevice));
    WdfWaitLockAcquire(global->Lock, NULL);
    if (RamFanEndHardwareTransactionState(&global->ActiveUsers)) {
        KeSetEvent(&global->ActiveUsersZero, IO_NO_INCREMENT, FALSE);
    }
    WdfWaitLockRelease(global->Lock);
}

static VOID
RamFanResetPnpContext(RAMFAN_PNP_CONTEXT *Context)
{
    RtlZeroMemory(Context, sizeof(*Context));
}

static UCHAR
RamFanQueryExpectedRole(WDFDEVICE Device)
{
    WDFMEMORY memory;
    WDF_DEVICE_PROPERTY_DATA propertyData;
    DEVPROPTYPE propertyType;
    PWCHAR instanceId;
    UNICODE_STRING instance;
    UNICODE_STRING smbusInstance = RTL_CONSTANT_STRING(L"ACPI\\PNP0C02\\700");
    UNICODE_STRING nctInstance = RTL_CONSTANT_STRING(L"ACPI\\PNP0C02\\0");
    NTSTATUS status;

    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &RamFanDeviceInstanceIdKey);
    status = WdfDeviceAllocAndQueryPropertyEx(Device,
                                               &propertyData,
                                               NonPagedPoolNx,
                                               WDF_NO_OBJECT_ATTRIBUTES,
                                               &memory,
                                               &propertyType);
    if (!NT_SUCCESS(status)) {
        return RAMFAN_ROLE_NONE;
    }

    if (propertyType != DEVPROP_TYPE_STRING) {
        WdfObjectDelete(memory);
        return RAMFAN_ROLE_NONE;
    }

    instanceId = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);
    if (instanceId == NULL) {
        WdfObjectDelete(memory);
        return RAMFAN_ROLE_NONE;
    }

    RtlInitUnicodeString(&instance, instanceId);
    if (RtlEqualUnicodeString(&instance, &smbusInstance, TRUE)) {
        WdfObjectDelete(memory);
        return RAMFAN_ROLE_SMBUS;
    }
    if (RtlEqualUnicodeString(&instance, &nctInstance, TRUE)) {
        WdfObjectDelete(memory);
        return RAMFAN_ROLE_NCT;
    }

    WdfObjectDelete(memory);
    return RAMFAN_ROLE_NONE;
}

static VOID
RamFanRegisterResources(WDFDEVICE Device, RAMFAN_PNP_CONTEXT *Context)
{
    RAMFAN_GLOBAL_CONTEXT *global;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(Device));
    WdfWaitLockAcquire(global->Lock, NULL);

    if (Context->Resources.Role == RAMFAN_ROLE_SMBUS) {
        if (global->SmbusConflict) {
            /* 资源所有权冲突在本次驱动生命周期内保持不可用。 */
        } else if (global->SmbusReady && global->SmbusDevice != Device) {
            global->SmbusConflict = TRUE;
            global->SmbusReady = FALSE;
            /* 保留 owner/ref，交由原 owner 的 ReleaseHardware 配对释放。 */
        } else {
            global->SmbusDevice = Device;
            global->SmbusStart = Context->Resources.SmbusStart;
            global->SmbusLength = Context->Resources.SmbusLength;
            /* SIO 授权随覆盖它的角色实例登记（实机为 \\700 的 SMBUS 角色）；
               只有完整覆盖且不歧义才记录，避免覆盖他处已登记值。 */
            if (Context->Resources.StandardSioPresent &&
                !Context->Resources.StandardSioAmbiguous) {
                global->StandardSioStart = Context->Resources.StandardSioStart;
                global->StandardSioLength = Context->Resources.StandardSioLength;
            }
            global->SmbusReady = TRUE;
            WdfObjectReference(Device);
            Context->RegisteredReference = TRUE;
        }
    } else if (Context->Resources.Role == RAMFAN_ROLE_NCT) {
        if (global->NctConflict) {
            /* 资源所有权冲突在本次驱动生命周期内保持不可用。 */
        } else if (global->NctReady && global->NctDevice != Device) {
            global->NctConflict = TRUE;
            global->NctReady = FALSE;
            /* 保留 owner/ref，交由原 owner 的 ReleaseHardware 配对释放。 */
        } else {
            global->NctDevice = Device;
            global->NctStart = Context->Resources.NctStart;
            global->NctLength = Context->Resources.NctLength;
            global->NctReady = TRUE;
            WdfObjectReference(Device);
            Context->RegisteredReference = TRUE;
        }
    }

    WdfWaitLockRelease(global->Lock);
}

static VOID
RamFanUnregisterResources(WDFDEVICE Device, RAMFAN_PNP_CONTEXT *Context)
{
    RAMFAN_GLOBAL_CONTEXT *global;
    BOOLEAN waitForUsers = FALSE;

    global = RamFanGetGlobalContext(WdfDeviceGetDriver(Device));
    WdfWaitLockAcquire(global->Lock, NULL);

    if (Context->Resources.Role == RAMFAN_ROLE_SMBUS &&
        global->SmbusDevice == Device) {
        waitForUsers = TRUE;
        global->SmbusDevice = NULL;
        global->SmbusStart = 0;
        global->SmbusLength = 0;
        /* SIO 授权由登记它的实例持有；该实例释放时一并清空。 */
        if (Context->Resources.StandardSioPresent &&
            !Context->Resources.StandardSioAmbiguous) {
            global->StandardSioStart = 0;
            global->StandardSioLength = 0;
        }
        global->SmbusReady = FALSE;
        /* 冲突一旦发生，本次驱动生命周期内不重新授权。 */
    } else if (Context->Resources.Role == RAMFAN_ROLE_NCT &&
               global->NctDevice == Device) {
        waitForUsers = TRUE;
        global->NctDevice = NULL;
        global->NctStart = 0;
        global->NctLength = 0;
        global->NctReady = FALSE;
        /* 冲突一旦发生，本次驱动生命周期内不重新授权。 */
    }

    WdfWaitLockRelease(global->Lock);

    if (waitForUsers) {
        KeWaitForSingleObject(&global->ActiveUsersZero,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
    }

    if (Context->RegisteredReference) {
        Context->RegisteredReference = FALSE;
        WdfObjectDereference(Device);
    }
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

    KeInitializeEvent(&global->ActiveUsersZero, NotificationEvent, TRUE);
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
    UCHAR expectedRole;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    context = RamFanGetPnpContext(Device);
    RamFanUnregisterResources(Device, context);
    RamFanResetPnpContext(context);
    if (ResourcesTranslated == NULL) {
        return STATUS_SUCCESS;
    }

    expectedRole = RamFanQueryExpectedRole(Device);

    for (descriptorIndex = 0;
         descriptorIndex < WdfCmResourceListGetCount(ResourcesTranslated);
         descriptorIndex++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;

        descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated,
                                                    descriptorIndex);
        if (descriptor == NULL || descriptor->Type != CmResourceTypePort) {
            continue;
        }

        if (descriptor->u.Port.Start.QuadPart < 0) {
            continue;
        }
        RamFanClassifyPortResource(&context->Resources,
                                   (ULONGLONG)descriptor->u.Port.Start.QuadPart,
                                   descriptor->u.Port.Length);
    }

    RamFanResourceMapSelectRole(&context->Resources, expectedRole);
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
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
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
    RAMFAN_TRANSACTION transaction;

    if (RamFanBeginHardwareTransaction(WdfIoQueueGetDevice(ext->Queue),
                                        &transaction)) {
        /* 真实事务接入前保持硬件访问门禁；事务生命周期仍已成对闭合。 */
        UNREFERENCED_PARAMETER(transaction);
        RamFanEndHardwareTransaction(WdfIoQueueGetDevice(ext->Queue));
    }
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
