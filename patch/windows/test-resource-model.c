/* test-resource-model.c — 资源分类 host-only 自检 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "driver/resource_model.h"
#include "driver/ramfan_ioctl.h"

static void
classify(RAMFAN_RESOURCE_MAP *map,
         unsigned long long start,
         unsigned long length)
{
    RamFanClassifyPortResource(map, start, length);
}

int
main(void)
{
    RAMFAN_RESOURCE_MAP map;

    RamFanResourceMapReset(&map);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START,
             RAMFAN_SMBUS_RESOURCE_LENGTH);
    RamFanResourceMapSelectRole(&map, RAMFAN_ROLE_SMBUS);
    assert(map.SmbusPresent && !map.SmbusAmbiguous);
    assert(map.Role == RAMFAN_ROLE_SMBUS);

    RamFanResourceMapReset(&map);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START - 1, 2);
    assert(!map.SmbusPresent && map.SmbusAmbiguous);

    RamFanResourceMapReset(&map);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START,
             RAMFAN_SMBUS_RESOURCE_LENGTH);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START,
             RAMFAN_SMBUS_RESOURCE_LENGTH);
    assert(map.SmbusPresent && map.SmbusAmbiguous);
    /* 仿 PNP0C02\\700：SMBus + 标准 SIO（0x22-0x3F 覆盖 0x2e/0x2f）+ 无关范围。 */
    RamFanResourceMapReset(&map);
    classify(&map, 0x0010, 0x10);
    classify(&map, 0x0022, 0x1e);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START,
             RAMFAN_SMBUS_RESOURCE_LENGTH);
    RamFanResourceMapSelectRole(&map, RAMFAN_ROLE_SMBUS);
    assert(map.SmbusPresent && !map.SmbusAmbiguous);
    assert(map.StandardSioPresent && !map.StandardSioAmbiguous);
    assert(map.Role == RAMFAN_ROLE_SMBUS);

    /* 仿 PNP0C02\\0：只含 NCT 自定义口 0x290，无标准 SIO。 */
    RamFanResourceMapReset(&map);
    classify(&map, 0x0200, 0x40);
    classify(&map, RAMFAN_NCT_RESOURCE_START,
             RAMFAN_NCT_RESOURCE_LENGTH);
    RamFanResourceMapSelectRole(&map, RAMFAN_ROLE_NCT);
    assert(map.NctPresent && !map.NctAmbiguous);
    assert(!map.StandardSioPresent);
    assert(map.Role == RAMFAN_ROLE_NCT);
    RamFanResourceMapReset(&map);
    classify(&map, RAMFAN_SMBUS_RESOURCE_START,
             RAMFAN_SMBUS_RESOURCE_LENGTH);
    classify(&map, RAMFAN_NCT_RESOURCE_START,
             RAMFAN_NCT_RESOURCE_LENGTH);
    classify(&map, RAMFAN_STANDARD_SIO_RESOURCE_START,
             RAMFAN_STANDARD_SIO_RESOURCE_LENGTH);
    RamFanResourceMapSelectRole(&map, RAMFAN_ROLE_NCT);
    assert(map.SmbusPresent && map.NctPresent && map.StandardSioPresent);
    assert(map.Role == RAMFAN_ROLE_NONE);

    /* 已知 PNP0C02\0 的 0x0200-0x023f 不授权 NCT/SIO 目标。 */
    RamFanResourceMapReset(&map);
    classify(&map, 0x0200, 0x40);
    RamFanResourceMapSelectRole(&map, RAMFAN_ROLE_NCT);
    assert(!map.NctPresent && !map.StandardSioPresent);
    assert(map.Role == RAMFAN_ROLE_NONE);

    assert(RamFanCanBeginHardwareTransaction(1, 0, 1, 0));
    assert(!RamFanCanBeginHardwareTransaction(0, 0, 1, 0));
    assert(!RamFanCanBeginHardwareTransaction(1, 1, 1, 0));
    assert(!RamFanCanBeginHardwareTransaction(1, 0, 0, 0));
    assert(!RamFanCanBeginHardwareTransaction(1, 0, 1, 1));

    assert(ULLONG_MAX == ~0ULL);
    RamFanResourceMapReset(&map);
    classify(&map, ULLONG_MAX, 1);
    classify(&map, ULLONG_MAX - 1, 3);
    assert(!map.SmbusPresent && !map.NctPresent &&
           !map.StandardSioPresent);

    {
        unsigned long activeUsers = 0;
        assert(RamFanBeginHardwareTransactionState(1, 0, 1, 0,
                                                   &activeUsers));
        assert(activeUsers == 1);
        /* Release 一侧后，新事务必须被拒绝，旧事务仍计数。 */
        assert(!RamFanBeginHardwareTransactionState(0, 0, 1, 0,
                                                    &activeUsers));
        assert(activeUsers == 1);
        assert(RamFanEndHardwareTransactionState(&activeUsers));
        assert(activeUsers == 0);
        assert(RamFanEndHardwareTransactionState(&activeUsers) == 0);
    }

    puts("resource model checks passed; no hardware access performed.");
    return 0;
}
