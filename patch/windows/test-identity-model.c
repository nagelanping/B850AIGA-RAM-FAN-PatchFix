/* test-identity-model.c — 身份门禁判定纯逻辑宿主自检（无 WDF、无端口）
 * 编译并链接 driver/identity_model.c，断言判定表。
 * 通过条件：CtrlFound=1 且 chip id 0xd8/0x02 → HwMatched=1；
 * 任一缺失（未找到控制器、0xffff、非预期 chip id）→ HwMatched=0。
 */
#include <stdio.h>
#include "driver/identity_model.h"

static int g_failures;

static void
AssertIdentity(int index,
               unsigned char ctrlFound,
               unsigned char hi,
               unsigned char lo,
               unsigned char expectValid,
               unsigned char expectMatched)
{
    RAMFAN_IDENTITY_INPUT in;
    RAMFAN_IDENTITY_OUTPUT out;

    in.ControllerFound = ctrlFound;
    in.ChipIdHi = hi;
    in.ChipIdLo = lo;
    RamFanEvaluateIdentity(&in, &out);

    if (out.ChipIdValid != expectValid || out.HwMatched != expectMatched) {
        printf("FAIL case %d: found=%u hi=%02x lo=%02x -> valid=%u matched=%u "
               "(expect %u/%u)\n",
               index, ctrlFound, hi, lo,
               out.ChipIdValid, out.HwMatched,
               expectValid, expectMatched);
        g_failures++;
    }
}

int
main(void)
{
    /* 0: 全部匹配 */
    AssertIdentity(0, 1, 0xd8, 0x02, 1, 1);
    /* 1: 控制器未找到 */
    AssertIdentity(1, 0, 0xd8, 0x02, 1, 0);
    /* 2: chip id 全 0xff（探针失败） */
    AssertIdentity(2, 1, 0xff, 0xff, 0, 0);
    /* 3: 非预期 chip id */
    AssertIdentity(3, 1, 0xd8, 0x03, 1, 0);
    AssertIdentity(4, 1, 0xd9, 0x02, 1, 0);
    /* 4: 半 0xff 视为探针失败 */
    AssertIdentity(5, 1, 0xd8, 0xff, 0, 0);
    AssertIdentity(6, 1, 0xff, 0x02, 0, 0);
    /* 5: 控制器未找到 + 探针失败 */
    AssertIdentity(7, 0, 0xff, 0xff, 0, 0);

    if (g_failures != 0) {
        printf("identity-model: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("identity-model checks passed; no hardware access performed.\n");
    return 0;
}
