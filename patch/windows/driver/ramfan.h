#pragma once

// ramfan.h — 驱动侧声明（PnP 资源识别骨架）
// 共享常量/IOCTL 定义在 ramfan_ioctl.h（服务也包含）。

#include <ntddk.h>
#include <wdf.h>
#include "ramfan_ioctl.h"

/* ---- 回调前置声明 ---- */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RamFanEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RamFanEvtIoDeviceControl;
EVT_WDF_FILE_CLOSE RamFanEvtFileClose;
EVT_WDF_DEVICE_PREPARE_HARDWARE RamFanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RamFanEvtReleaseHardware;
NTSTATUS RamFanCreateDevice(WDFDRIVER Driver);
