/* identity_model.c — 身份门禁判定纯逻辑 */
#include "identity_model.h"
#include "ramfan_ioctl.h"   /* NCT_EXPECTED_CHIP_ID_HI/LO */

void
RamFanEvaluateIdentity(const RAMFAN_IDENTITY_INPUT *in,
                       RAMFAN_IDENTITY_OUTPUT *out)
{
    /* 拒绝式：任一字节 0xff 视为探针失败（0xff 表示该字节读不到） */
    out->ChipIdValid = (in->ChipIdHi != 0xff) && (in->ChipIdLo != 0xff);
    out->HwMatched = in->ControllerFound &&
                     out->ChipIdValid &&
                     in->ChipIdHi == NCT_EXPECTED_CHIP_ID_HI &&
                     in->ChipIdLo == NCT_EXPECTED_CHIP_ID_LO;
}
