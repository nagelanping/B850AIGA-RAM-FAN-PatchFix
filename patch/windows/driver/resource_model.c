/* resource_model.c — translated port 资源分类纯逻辑 */
#include "resource_model.h"
#include "ramfan_ioctl.h"

static void
RamFanRememberPort(unsigned char *present,
                   unsigned long *start,
                   unsigned long *length,
                   unsigned char *ambiguous,
                   unsigned long targetStart,
                   unsigned long targetLength)
{
    if (!*present) {
        *present = 1;
        *start = targetStart;
        *length = targetLength;
    } else {
        *ambiguous = 1;
    }
}

static void
RamFanClassifyTarget(unsigned long long start,
                     unsigned long long end,
                     unsigned long targetStart,
                     unsigned long targetLength,
                     unsigned char *present,
                     unsigned long *savedStart,
                     unsigned long *savedLength,
                     unsigned char *ambiguous)
{
    unsigned long long targetEnd =
        (unsigned long long)targetStart + targetLength - 1;

    if (end < targetStart || start > targetEnd) {
        return;
    }
    if (start <= targetStart && end >= targetEnd) {
        RamFanRememberPort(present,
                           savedStart,
                           savedLength,
                           ambiguous,
                           targetStart,
                           targetLength);
    } else {
        *ambiguous = 1;
    }
}

void
RamFanResourceMapReset(RAMFAN_RESOURCE_MAP *map)
{
    *map = (RAMFAN_RESOURCE_MAP){0};
}

void
RamFanClassifyPortResource(RAMFAN_RESOURCE_MAP *map,
                           unsigned long long start,
                           unsigned long length)
{
    unsigned long long end;

    if (length == 0 || start > (~0ULL) - (length - 1)) {
        return;
    }
    end = start + length - 1;

    RamFanClassifyTarget(start,
                         end,
                         RAMFAN_SMBUS_RESOURCE_START,
                         RAMFAN_SMBUS_RESOURCE_LENGTH,
                         &map->SmbusPresent,
                         &map->SmbusStart,
                         &map->SmbusLength,
                         &map->SmbusAmbiguous);
    RamFanClassifyTarget(start,
                         end,
                         RAMFAN_NCT_RESOURCE_START,
                         RAMFAN_NCT_RESOURCE_LENGTH,
                         &map->NctPresent,
                         &map->NctStart,
                         &map->NctLength,
                         &map->NctAmbiguous);
    RamFanClassifyTarget(start,
                         end,
                         RAMFAN_STANDARD_SIO_RESOURCE_START,
                         RAMFAN_STANDARD_SIO_RESOURCE_LENGTH,
                         &map->StandardSioPresent,
                         &map->StandardSioStart,
                         &map->StandardSioLength,
                         &map->StandardSioAmbiguous);
}

void
RamFanResourceMapSelectRole(RAMFAN_RESOURCE_MAP *map,
                            unsigned char expectedRole)
{
    map->Role = RAMFAN_ROLE_NONE;
    if (expectedRole == RAMFAN_ROLE_SMBUS &&
        map->SmbusPresent && !map->SmbusAmbiguous &&
        !map->NctPresent && !map->NctAmbiguous) {
        map->Role = RAMFAN_ROLE_SMBUS;
    } else if (expectedRole == RAMFAN_ROLE_NCT &&
               map->NctPresent && !map->NctAmbiguous &&
               !map->SmbusPresent && !map->SmbusAmbiguous) {
        map->Role = RAMFAN_ROLE_NCT;
    }
}

unsigned char
RamFanCanBeginHardwareTransaction(unsigned char smbusReady,
                                  unsigned char smbusConflict,
                                  unsigned char nctReady,
                                  unsigned char nctConflict)
{
    return smbusReady && !smbusConflict && nctReady && !nctConflict;
}

unsigned char
RamFanBeginHardwareTransactionState(unsigned char smbusReady,
                                    unsigned char smbusConflict,
                                    unsigned char nctReady,
                                    unsigned char nctConflict,
                                    unsigned long *activeUsers)
{
    if (!RamFanCanBeginHardwareTransaction(smbusReady,
                                           smbusConflict,
                                           nctReady,
                                           nctConflict) ||
        *activeUsers == ~0UL) {
        return 0;
    }
    ++*activeUsers;
    return 1;
}

unsigned char
RamFanEndHardwareTransactionState(unsigned long *activeUsers)
{
    if (*activeUsers == 0) {
        return 0;
    }
    return --*activeUsers == 0;
}
