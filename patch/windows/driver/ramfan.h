#pragma once

// ramfan.h — 驱动侧声明（非 PnP 控制设备 + 身份门禁 + 受控 SMBus 读取）
// 2026-09-05 批准模型。共享常量/IOCTL 定义在 ramfan_ioctl.h（服务也包含）。

#include <ntddk.h>
#include <wdf.h>
#include "ramfan_ioctl.h"
#include "identity_model.h"

/* ---- 回调前置声明 ---- */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD RamFanEvtDriverUnload;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RamFanEvtIoDeviceControl;
EVT_WDF_FILE_CLOSE RamFanEvtFileClose;
NTSTATUS RamFanCreateDevice(WDFDRIVER Driver);

/* ---- hw.c ---- */
/* 确认 FCH SMBus 控制器存在（系统 PnP 枚举）。baseOut 为固定目标基址（仅当找到时）。 */
NTSTATUS RamFanProbeFchSmbusController(BOOLEAN *foundOut, USHORT *baseOut);

/* NCT chip id：标准 SIO 0x2e/0x2f 解锁→读 0x20/0x21→锁定。只在驱动内部执行。 */
NTSTATUS RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo);

/* SMBus HST word read（基址白名单 0xb00，§5.2 第 2 步）。
 * hstStsOut 回传原始 HST_STS 供分类。调用方必须已过身份门禁并串行调用。 */
NTSTATUS RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd,
                            USHORT *rawOut, UCHAR *hstStsOut);

/* 温度换算：((raw<<3)>>5)*25/100，返回 ULONG 供调用方做范围校验 */
ULONG RamFanCelsiusFromRaw(USHORT raw);
