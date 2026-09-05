/* ramfan.c — B850AIGA RAM-FAN 非 PnP 控制设备驱动（只读身份门禁）
 *
 * 2026-09-05 机主批准受控非 PnP 访问模型（见 AGENTS.md“授权边界”）。
 * PnP 绑定（upper-filter / function-driver）已证伪移除，不再接收
 * EvtDeviceAdd/translated resources。本驱动以普通内核服务方式加载
 * （sc create type= kernel），创建控制设备 \Device\RamFanVirtTemp。
 *
 * 当前阶段（§5.2 第 1 步）只实现只读身份门禁：
 *   - IOCTL_RAMFAN_QUERY_HW：PCI DEV_790B 存在性 + NCT chip id（0x2e/0x2f），
 *     判定 HwMatched；不访问 SMBus 事务寄存器、不写 NCT。
 *   - IOCTL_RAMFAN_READ_DIMM_TEMP / FEED_ONCE：保持阻断（SMBus 试验与写回
 *     需后续单独批准）。
 */
#include "ramfan.h"

typedef struct _RAMFAN_DRIVER_CONTEXT {
    WDFWAITLOCK Lock;          /* 保护活动计数/卸载标记 */
    WDFDEVICE ControlDevice;   /* 手动创建的控制设备 */
    LONG ActiveUsers;
    KEVENT ActiveUsersZero;
    BOOLEAN Removing;          /* EvtDriverUnload 已开始 */
} RAMFAN_DRIVER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_DRIVER_CONTEXT, RamFanGetDriverContext);

typedef struct _RAMFAN_DEVICE_EXTENSION {
    WDFQUEUE Queue;
} RAMFAN_DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RAMFAN_DEVICE_EXTENSION, RamFanGetDeviceContext);

/* ---- 活动用户 rundown：IOCTL 与 DriverUnload 互斥 ---- */
static BOOLEAN
RamFanBeginIo(WDFDEVICE Device)
{
    RAMFAN_DRIVER_CONTEXT *ctx =
        RamFanGetDriverContext(WdfDeviceGetDriver(Device));
    BOOLEAN ok = FALSE;

    WdfWaitLockAcquire(ctx->Lock, NULL);
    if (!ctx->Removing) {
        ++ctx->ActiveUsers;
        if (ctx->ActiveUsers == 1) {
            KeClearEvent(&ctx->ActiveUsersZero);
        }
        ok = TRUE;
    }
    WdfWaitLockRelease(ctx->Lock);
    return ok;
}

static VOID
RamFanEndIo(WDFDEVICE Device)
{
    RAMFAN_DRIVER_CONTEXT *ctx =
        RamFanGetDriverContext(WdfDeviceGetDriver(Device));

    WdfWaitLockAcquire(ctx->Lock, NULL);
    if (ctx->ActiveUsers > 0 && --ctx->ActiveUsers == 0) {
        KeSetEvent(&ctx->ActiveUsersZero, IO_NO_INCREMENT, FALSE);
    }
    WdfWaitLockRelease(ctx->Lock);
}

/* ---- QUERY_HW：只读身份门禁 ---- */
static NTSTATUS
RamFanQueryHw(RAMFAN_QUERY_HW_OUT *out)
{
    RAMFAN_IDENTITY_INPUT in;
    RAMFAN_IDENTITY_OUTPUT eval;
    BOOLEAN controllerFound = FALSE;
    USHORT base = 0;
    UCHAR hi = 0xff;
    UCHAR lo = 0xff;

    RtlZeroMemory(out, sizeof(*out));

    /* 白名单只读探针：PCI 配置读取 + 标准 SIO 0x2e/0x2f chip id。 */
    RamFanProbeFchSmbusController(&controllerFound, &base);
    RamFanProbeNctChipId(&hi, &lo);

    in.ControllerFound = controllerFound ? 1 : 0;
    in.ChipIdHi = hi;
    in.ChipIdLo = lo;
    RamFanEvaluateIdentity(&in, &eval);

    out->SmbusBase = base;
    out->ChipIdHi = hi;
    out->ChipIdLo = lo;
    out->ControllerFound = in.ControllerFound;
    out->ChipIdValid = eval.ChipIdValid;
    out->HwMatched = eval.HwMatched;
    out->Reserved = 0;
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
    RAMFAN_DRIVER_CONTEXT *ctx;
    WDF_IO_QUEUE_CONFIG queueConfig;
    NTSTATUS status;
    UNICODE_STRING dosName, devName, sddl;
    WDF_FILEOBJECT_CONFIG fileConfig;

    /* 控制设备：SDDL 限 SYSTEM/管理员 */
    RtlInitUnicodeString(&sddl, RAMFAN_DEVICE_SDDL);
    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 缓冲型 IOCTL + 独占（同一时刻仅一个打开句柄，配合串行队列） */
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, TRUE);

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
        WdfObjectDelete(device);
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
        WdfObjectDelete(device);
        return status;
    }

    WdfDeviceConfigureRequestDispatching(device, ext->Queue,
                                         WdfRequestTypeDeviceControl);
    ctx = RamFanGetDriverContext(Driver);
    ctx->ControlDevice = device;
    WdfControlFinishInitializing(device);

    return STATUS_SUCCESS;
}

/* ---- DriverEntry（非 PnP：无 EvtDeviceAdd） ---- */
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDRIVER driver;
    RAMFAN_DRIVER_CONTEXT *ctx;
    NTSTATUS status;

    /* 非 PnP 驱动：EvtDriverDeviceAdd = NULL */
    WDF_DRIVER_CONFIG_INIT(&config, NULL);
    config.EvtDriverUnload = RamFanEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attrs, RAMFAN_DRIVER_CONTEXT);
    status = WdfDriverCreate(DriverObject, RegistryPath,
                             &attrs, &config, &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = RamFanGetDriverContext(driver);
    RtlZeroMemory(ctx, sizeof(*ctx));

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = driver;
    status = WdfWaitLockCreate(&attrs, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    KeInitializeEvent(&ctx->ActiveUsersZero, NotificationEvent, TRUE);

    return RamFanCreateDevice(driver);
}

/* ---- EvtDriverUnload：等活动用户归零后删除控制设备 ---- */
VOID
RamFanEvtDriverUnload(WDFDRIVER Driver)
{
    RAMFAN_DRIVER_CONTEXT *ctx = RamFanGetDriverContext(Driver);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    ctx->Removing = TRUE;
    WdfWaitLockRelease(ctx->Lock);

    for (;;) {
        LONG users;
        WdfWaitLockAcquire(ctx->Lock, NULL);
        users = ctx->ActiveUsers;
        WdfWaitLockRelease(ctx->Lock);
        if (users == 0) {
            break;
        }
        KeWaitForSingleObject(&ctx->ActiveUsersZero,
                              Executive, KernelMode, FALSE, NULL);
    }
    if (ctx->ControlDevice != NULL) {
        WdfObjectDelete(ctx->ControlDevice);
        ctx->ControlDevice = NULL;
    }
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
    WDFDEVICE device;

    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);

    if (!RamFanBeginIo(device)) {
        /* 驱动正在卸载 */
        WdfRequestCompleteWithInformation(Request, STATUS_DELETE_PENDING, 0);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_RAMFAN_QUERY_HW: {
        RAMFAN_QUERY_HW_OUT out = {0};

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = RamFanQueryHw(&out);
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

    case IOCTL_RAMFAN_READ_DIMM_TEMP:
        /* SMBus 试验未批准：保持阻断 */
        status = STATUS_DEVICE_NOT_READY;
        break;

    case IOCTL_RAMFAN_FEED_ONCE: {
        RAMFAN_FEED_ONCE_OUT out = {0};

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        out.Status = RAMFAN_FEED_HW_UNAVAILABLE; /* 写回未批准 */
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
        status == STATUS_BUFFER_TOO_SMALL ||
        status == STATUS_DEVICE_NOT_READY) {
        /* 先完成请求再减活动计数：避免 EndIo 归零后、Complete 前
           设备被 EvtDriverUnload 删除造成二次完成。 */
        WdfRequestCompleteWithInformation(Request, status, 0);
        RamFanEndIo(device);
        return;
    }

    /* 成功路径已 SetInformation；先完成请求再减活动计数 */
    WdfRequestComplete(Request, status);
    RamFanEndIo(device);
}

/* ---- 文件/设备清理 ---- */
VOID
RamFanEvtFileClose(WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
}
