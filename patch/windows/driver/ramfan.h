#pragma once

// ramfan.h — 驱动侧声明（非 PnP 控制设备 + 只读身份门禁，2026-09-05 批准模型）
// 共享常量/IOCTL 定义在 ramfan_ioctl.h（服务也包含）。

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

/* ---- hw.c：只读身份探针（PCI DEV_790B 存在性 + NCT chip id）---- */
/* 找到 FCH SMBus 控制器（VEN_1022&DEV_790B）。baseOut 为 PCI BAR0 的 I/O
 * 基址（仅当 BAR 有效时写出）；不访问 SMBus 事务寄存器。 */
NTSTATUS RamFanProbeFchSmbusController(BOOLEAN *foundOut, USHORT *baseOut);

/* NCT chip id：标准 SIO 0x2e/0x2f 解锁→读 0x20/0x21→锁定。只在驱动内部执行。 */
NTSTATUS RamFanProbeNctChipId(UCHAR *hi, UCHAR *lo);
