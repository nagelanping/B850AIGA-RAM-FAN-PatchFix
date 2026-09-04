#pragma once

// ramfan_ioctl.h — 驱动与服务共享的 IOCTL 定义（阶段 1：只读）
// 用户态（服务）与内核态（驱动）都可包含，不依赖 ntddk.h/wdf.h。
// 所有常量来自 LOG.md 已确认事实，禁止改动温度源/曲线相关寄存器。

#ifdef __cplusplus
extern "C" {
#endif

// ---- 设备名称 ----

// 控制设备 SDDL：仅 SYSTEM 与内置管理员可打开（服务=SYSTEM，--once=管理员）
#define RAMFAN_DEVICE_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"
#define RAMFAN_DEVICE_NAME     L"\\Device\\RamFanVirtTemp"
#define RAMFAN_DOS_DEVICE_NAME L"\\DosDevices\\RamFanVirtTemp"
#define RAMFAN_WIN32_DEVICE    L"\\\\.\\RamFanVirtTemp"

// ---- SMBus HST 寄存器偏移（基址由 PCI/ACPI 资源确认，预期 0xb00）----
#define HST_STS_OFF   0x00
#define HST_CNT_OFF   0x02
#define HST_CMD_OFF   0x03
#define HST_ADD_OFF   0x04
#define HST_DAT0_OFF  0x05
#define HST_DAT1_OFF  0x06

// SMBus 状态位（实测：0x02 可出现在成功事务，不能单独判失败；0x04 为无设备/CRC）
#define HST_STS_BUSY  0x01
#define HST_STS_OK2   0x02
#define HST_STS_ERR   0x04

// word read + start（固件序列）
#define HST_CNT_WORD_READ 0x4c

// SPD 温度命令
#define SPD_CMD_TEMP 0x31

// SPD 7-bit 地址轮询顺序（LOG.md）
#define RAMFAN_SPD_ADDR_COUNT 4
#define RAMFAN_SPD_ADDR_53 0x53
#define RAMFAN_SPD_ADDR_52 0x52
#define RAMFAN_SPD_ADDR_51 0x51
#define RAMFAN_SPD_ADDR_50 0x50

// ---- NCT SIO 自定义端口（页选择与 Virtual_TEMP 写回，阶段 2 使用）----
#define NCT_SIO_IDX 0x295
#define NCT_SIO_DAT 0x296

// NCT 标准 SIO 端口（chip id 读取）
#define NCT_STD_IDX 0x2e
#define NCT_STD_DAT 0x2f

// 实测 chip id（NCT6796D-S / NCT6799D 兼容系列）
#define NCT_EXPECTED_CHIP_ID_HI 0xd8
#define NCT_EXPECTED_CHIP_ID_LO 0x02

// ---- 温度范围（LOG.md：0..120°C 有效）----
#define RAMFAN_TEMP_MIN 0
#define RAMFAN_TEMP_MAX 120

// ---- SMBus 事务超时（100 ms）----
#define RAMFAN_SMBUS_TIMEOUT_MS 100

// ---- 每个 DIMM 槽的结果 ----
typedef struct _RAMFAN_DIMM_RESULT {
    unsigned char Address;   // 7-bit SPD 地址
    unsigned char Status;    // 0=成功 1=NACK(空槽) 2=超时 3=总线错误 4=非法数据
    unsigned short Raw;      // 原始 word
    unsigned char Celsius;   // 换算温度（有效时）
} RAMFAN_DIMM_RESULT;

#define RAMFAN_DIMM_OK          0
#define RAMFAN_DIMM_NACK        1
#define RAMFAN_DIMM_TIMEOUT     2
#define RAMFAN_DIMM_BUS_ERR     3
#define RAMFAN_DIMM_BAD_DATA    4

// ---- IOCTL（阶段 1：只读；阶段 2 才加 FEED_ONCE，禁止裸端口暴露）----
#define RAMFAN_IOCTL_BASE FILE_DEVICE_UNKNOWN

// 输出：SMBus 基址、chip id、硬件匹配标志
typedef struct _RAMFAN_QUERY_HW_OUT {
    unsigned short SmbusBase;   // 从 PCI BAR 确认的基址（预期 0xb00）
    unsigned char  ChipIdHi;
    unsigned char  ChipIdLo;
    unsigned char  HwMatched;   // 非零=硬件匹配（SMBus 基址有效且 chip id 匹配）
} RAMFAN_QUERY_HW_OUT;

// 输出：全部候选槽读取结果 + 最高有效温度
typedef struct _RAMFAN_READ_DIMM_OUT {
    unsigned char  Count;                 // 返回的槽数（<= RAMFAN_SPD_ADDR_COUNT）
    unsigned char  MaxCelsius;            // 所有成功槽的最高温度（无成功则 0）
    unsigned char  AnySuccess;            // 非零=至少一个槽成功
    RAMFAN_DIMM_RESULT Slots[RAMFAN_SPD_ADDR_COUNT];
} RAMFAN_READ_DIMM_OUT;

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif
#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#define IOCTL_RAMFAN_QUERY_HW \
    CTL_CODE(RAMFAN_IOCTL_BASE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RAMFAN_READ_DIMM_TEMP \
    CTL_CODE(RAMFAN_IOCTL_BASE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#ifdef __cplusplus
}
#endif
