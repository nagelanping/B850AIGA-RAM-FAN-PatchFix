#pragma once
/*
 * identity_model.h — 身份门禁判定纯逻辑（无 WDF / 无端口依赖）。
 *
 * 2026-09-05 非 PnP 受控模型：身份依据为 PCI VEN_1022&DEV_790B（FCH SMBus
 * 控制器）存在 + NCT chip id == 0xd802（NCT6796D-S 兼容系列）。ACPI 声明与
 * 主板型号作为目标机静态证据记录，不进入本函数。判定只做拒绝式匹配。
 */

typedef struct _RAMFAN_IDENTITY_INPUT {
    unsigned char ControllerFound;   /* 1=PCI 0x1022:0x790B 找到 */
    unsigned char ChipIdHi;          /* 标准 SIO 0x2e/0x2f 探针结果 */
    unsigned char ChipIdLo;
} RAMFAN_IDENTITY_INPUT;

typedef struct _RAMFAN_IDENTITY_OUTPUT {
    unsigned char ChipIdValid;       /* 1=探针成功（非 0xffff） */
    unsigned char HwMatched;         /* 1=ControllerFound && chip id==0xd802 */
} RAMFAN_IDENTITY_OUTPUT;

void
RamFanEvaluateIdentity(const RAMFAN_IDENTITY_INPUT *in,
                       RAMFAN_IDENTITY_OUTPUT *out);
