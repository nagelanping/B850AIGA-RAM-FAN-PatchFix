#pragma once

/*
 * resource_model.h — 无 WDF 依赖的 translated port 资源分类。
 *
 * 该模型只处理已经确认是 port descriptor 的资源；调用方负责过滤
 * descriptor 类型及负的物理地址。它不表示端口已获得独占访问权。
 */

typedef struct _RAMFAN_RESOURCE_MAP {
    unsigned char SmbusPresent;
    unsigned char NctPresent;
    unsigned char StandardSioPresent;
    unsigned char SmbusAmbiguous;
    unsigned char NctAmbiguous;
    unsigned char StandardSioAmbiguous;
    unsigned long SmbusStart;
    unsigned long SmbusLength;
    unsigned long NctStart;
    unsigned long NctLength;
    unsigned long StandardSioStart;
    unsigned long StandardSioLength;
    unsigned char Role;
} RAMFAN_RESOURCE_MAP;

#define RAMFAN_ROLE_NONE  0
#define RAMFAN_ROLE_SMBUS 1
#define RAMFAN_ROLE_NCT   2

void
RamFanResourceMapReset(RAMFAN_RESOURCE_MAP *map);

void
RamFanClassifyPortResource(RAMFAN_RESOURCE_MAP *map,
                           unsigned long long start,
                           unsigned long length);

void
RamFanResourceMapSelectRole(RAMFAN_RESOURCE_MAP *map,
                            unsigned char expectedRole);

unsigned char
RamFanCanBeginHardwareTransaction(unsigned char smbusReady,
                                  unsigned char smbusConflict,
                                  unsigned char nctReady,
                                  unsigned char nctConflict);

unsigned char
RamFanBeginHardwareTransactionState(unsigned char smbusReady,
                                    unsigned char smbusConflict,
                                    unsigned char nctReady,
                                    unsigned char nctConflict,
                                    unsigned long *activeUsers);

unsigned char
RamFanEndHardwareTransactionState(unsigned long *activeUsers);
