/* ramfan.c — B850AIGA RAM-FAN 非 PnP 控制设备驱动（身份门禁 + 受控 SMBus 读取）
 *
 * 2026-09-05 机主批准受控非 PnP 访问模型（见 AGENTS.md“授权边界”），
 * 并批准 §5.2 第 2 步受控 SMBus 试验（读 DIMM；见 LOG.md 2026-09-05 决策记录）。
 * PnP 绑定（upper-filter / function-driver）已证伪移除，不再接收
 * EvtDeviceAdd/translated resources。本驱动以普通内核服务方式加载
 * （sc create type= kernel），创建控制设备 \Device\RamFanVirtTemp。
 *
 * 阶段（§5.2）：
 *   - 第 1 步身份门禁（已实机验证）：IOCTL_RAMFAN_QUERY_HW：DEV_790B 存在性
 *     （Enum\PCI 前缀枚举）+ NCT chip id（0x2e/0x2f）判定 HwMatched。
 *   - 第 2 步受控 SMBus 读取（已实机验证）：READ_DIMM_TEMP 读全部候选 SPD 槽；
 *     确认已装 0x53/0x51、空槽 0x52/0x50（BUS_ERR+0x06）。
 *   - 第 3 步单次写回（2026-09-05 机主批准）：FEED_ONCE 读取→校验→最高温→
 *     NCT page 0x0c/reg 0x36 写回并读回校验；页保存/恢复在 hw.c。
 *   - 常驻喂值阶段（2026-09-06）：服务端 0.5s 周期 FEED_ONCE 复用本驱动；
 *     方案 B：每轮全读 4 槽 + 屏蔽异常温度（0°C/越界），只取有效最高温。
 */
#include "ramfan.h"

typedef struct _RAMFAN_DRIVER_CONTEXT {
    WDFWAITLOCK Lock;          /* 保护活动计数/卸载标记 */
    WDFDEVICE ControlDevice;   /* 手动创建的控制设备 */
    LONG ActiveUsers;
    KEVENT ActiveUsersZero;
    BOOLEAN Removing;          /* EvtDriverUnload 已开始 */
    /* 无跨 IOCTL 状态：每轮全读 4 槽并屏蔽异常温度（0°C/越界）只取有效最高温。
     * 不做空槽隔离状态机——空槽偶发 BUS_ERR/0°C 垃圾被温度过滤排除，
     * 无永久累积污染路径（2026-09-06 机主方案 B）。 */
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

/* ---- 读 DIMM 槽温度（受控 SMBus，§5.2 第 2/3 步 + 常驻） ----
 * slotMask：bit i = 读 kSpdAddrs[i] 槽（0x53,0x52,0x51,0x50）。mask 位为 0 的槽
 * 不访问 SMBus（Status=UNCHECKED）。READ_DIMM_TEMP/FEED 当前都用全 mask
 * （0x0F）读全部候选；mask 参数保留供未来只读子集用。
 * 温度有效范围 RAMFAN_TEMP_MIN..MAX（1..120）：换算结果为 0°C 视为明显异常
 * （DDR 运行中不可能，通常为空槽偶发 raw=0 垃圾），归 BAD_DATA 不取用。
 * 前置：gateDone=FALSE 时做身份门禁（不匹配即拒，零事务）。
 * 事务基址固定为白名单 RAMFAN_SMBUS_RESOURCE_START。串行队列内调用。
 */
static NTSTATUS
RamFanReadDimmTemp(BOOLEAN gateDone, UCHAR slotMask,
                   RAMFAN_READ_DIMM_OUT *out)
{
    static const UCHAR kSpdAddrs[RAMFAN_SPD_ADDR_COUNT] = {
        RAMFAN_SPD_ADDR_53, RAMFAN_SPD_ADDR_52,
        RAMFAN_SPD_ADDR_51, RAMFAN_SPD_ADDR_50,
    };
    UCHAR maxC = 0;
    UCHAR anySuccess = 0;
    UCHAR count = 0;
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(out, sizeof(*out));

    if (!gateDone) {
        RAMFAN_QUERY_HW_OUT hw;
        status = RamFanQueryHw(&hw);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (!hw.HwMatched) {
            return STATUS_ACCESS_DENIED;
        }
    }

    for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
        RAMFAN_DIMM_RESULT *slot = &out->Slots[i];
        USHORT raw = 0;
        UCHAR hst = 0;
        ULONG c;

        slot->Address = kSpdAddrs[i];
        if (!(slotMask & (1u << i))) {
            slot->Status = RAMFAN_DIMM_UNCHECKED;
            continue;
        }

        status = RamFanSmbusReadWord(RAMFAN_SMBUS_RESOURCE_START,
                                     kSpdAddrs[i], SPD_CMD_TEMP,
                                     &raw, &hst);
        slot->HstSts = hst;

        if (NT_SUCCESS(status)) {
            c = RamFanCelsiusFromRaw(raw);
            if (c < RAMFAN_TEMP_MIN || c > RAMFAN_TEMP_MAX) {
                /* 成功事务但读数越界/异常（含 0°C）不可信 */
                slot->Status = RAMFAN_DIMM_BAD_DATA;
                slot->Raw = raw;
            } else {
                slot->Status = RAMFAN_DIMM_OK;
                slot->Raw = raw;
                slot->Celsius = (UCHAR)c;
                anySuccess = 1;
                if (c > maxC) {
                    maxC = (UCHAR)c;
                }
            }
        } else if (status == STATUS_IO_TIMEOUT ||
                   status == STATUS_DEVICE_BUSY) {
            slot->Status = RAMFAN_DIMM_TIMEOUT;
        } else if (status == STATUS_DATA_ERROR) {
            /* 0x04 无法区分空槽 NACK 与 CRC/总线异常（LOG 已确认） */
            slot->Status = RAMFAN_DIMM_BUS_ERR;
        } else {
            slot->Status = RAMFAN_DIMM_BUS_ERR;
        }
        count++;
    }

    out->Count = count;
    out->MaxCelsius = maxC;
    out->AnySuccess = anySuccess;
    return STATUS_SUCCESS;
}

/* ---- FEED_ONCE：全读候选槽 → 温度过滤 → 最高温 → NCT 写回并读回校验（常驻） ----
 * 身份门禁不匹配（Status=3）时不返回槽数据、不做任何事务。
 * 方案 B（2026-09-06 机主决定）：不做空槽隔离/映射状态机，每轮全读 4 槽；
 * 温度过滤是关键：RamFanReadDimmTemp 已把换算后 <RAMFAN_TEMP_MIN(1°C)
 * 或 >120°C 归 BAD_DATA，0°C/垃圾不会成为候选。空槽无论返回 BUS_ERR 还是
 * 偶发杂散数据都不会污染判断（每轮独立，无跨轮状态）。
 * 逐槽仅取 OK 槽中最高有效温度；无任何 OK 槽 → READ_FAILED 不写；
 * 写回读回不一致 → WRITE_FAILED。不写猜测值、不写 0°C。
 * 调用方（EvtIoDeviceControl）在驱动锁/串行队列内。
 */
static NTSTATUS
RamFanFeedOnce(RAMFAN_FEED_ONCE_OUT *out)
{
    RAMFAN_QUERY_HW_OUT hw;
    RAMFAN_READ_DIMM_OUT rd;
    UCHAR rb = 0;
    UCHAR maxC = 0;
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(out, sizeof(*out));
    for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
        out->Slots[i].Status = RAMFAN_DIMM_UNCHECKED;
    }

    /* 身份门禁：不匹配 → HW_MISMATCH，零事务 */
    status = RamFanQueryHw(&hw);
    if (!NT_SUCCESS(status) || !hw.HwMatched) {
        out->Status = RAMFAN_FEED_HW_MISMATCH;
        return STATUS_SUCCESS;
    }

    /* 全读 4 槽（含空槽；有效温度过滤在读取函数内完成） */
    status = RamFanReadDimmTemp(TRUE, 0x0F, &rd);
    if (!NT_SUCCESS(status)) {
        out->Status = RAMFAN_FEED_READ_FAILED;
        return STATUS_SUCCESS;
    }
    for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
        out->Slots[i] = rd.Slots[i];
        if (rd.Slots[i].Status == RAMFAN_DIMM_OK &&
            rd.Slots[i].Celsius > maxC) {
            maxC = rd.Slots[i].Celsius;
        }
    }

    if (maxC == 0 && !rd.AnySuccess) {
        /* 无有效温度样本（全 BUS_ERR/超时/BAD_DATA） */
        out->Status = RAMFAN_FEED_READ_FAILED;
        return STATUS_SUCCESS;
    }

    out->MaxCelsius = maxC;

    /* 写回并读回校验（端口固定白名单 0x295/0x296；页保存/恢复在 hw.c） */
    status = RamFanNctWriteVirtTemp(maxC, &rb);
    out->WrittenCelsius = maxC;
    out->ReadBackCelsius = rb;
    if (!NT_SUCCESS(status)) {
        out->Status = RAMFAN_FEED_WRITE_FAILED;
        return STATUS_SUCCESS;
    }
    out->Status = RAMFAN_FEED_OK;
    return STATUS_SUCCESS;
}
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

    case IOCTL_RAMFAN_READ_DIMM_TEMP: {
        RAMFAN_READ_DIMM_OUT out = {0};

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = RamFanReadDimmTemp(FALSE, 0x0F, &out);
        if (!NT_SUCCESS(status)) {
            /* 身份不匹配或不支持：不返回槽数据，也不做部分降速 */
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


    case IOCTL_RAMFAN_FEED_ONCE: {
        RAMFAN_FEED_ONCE_OUT out = {0};

        if (OutputBufferLength < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = RamFanFeedOnce(&out);
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
