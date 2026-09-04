#pragma once

// ramfan.h — 驱动侧声明（阶段 1：只读）
// 共享常量/IOCTL 定义在 ramfan_ioctl.h（服务也包含）。

#include <ntddk.h>
#include <wdf.h>
#include "ramfan_ioctl.h"

/* ---- 设备扩展与回调定义在 ramfan.c（仅驱动侧使用） ---- */

/* ---- 回调前置声明 ---- */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD RamFanEvtDriverUnload;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RamFanEvtIoDeviceControl;
EVT_WDF_FILE_CLOSE RamFanEvtFileClose;
NTSTATUS RamFanCreateDevice(WDFDRIVER Driver);
/* ---- hw.c 中实现 ---- */
NTSTATUS RamFanFindSmbusBase(USHORT *baseOut);
NTSTATUS RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo);
NTSTATUS RamFanSmbusReadWord(USHORT base, UCHAR addr7, UCHAR cmd,
                             USHORT *rawOut);
ULONG    RamFanCelsiusFromRaw(USHORT raw);
