/* ramfan-service.c — B850AIGA RAM-FAN Virtual_TEMP 补丁服务（受控试验，SMBus 读取阶段）
 *
 *   - --identity：只读身份门禁检查（QUERY_HW，不访问 SMBus、不写 NCT）
 *   - --dimm：受控 SMBus 读取实验（READ_DIMM_TEMP，读全部候选槽并打印状态；不写 NCT）
 *   - --once：单次写回（FEED_ONCE：读→校验→最高温→NCT Virtual_TEMP 写回，§5.2 第 3 步）
 *   - --install / --uninstall：SCM 安装仍拒绝（验收后才启用）
 *   - 默认 SCM 模式启动时执行一次身份门禁检查；常驻 0.5s 喂值留到后续阶段
 *
 * 构建：MSVC，链接 advapi32（SCM）与 kernel32。
 * 包含共享定义：../driver/ramfan_ioctl.h
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../driver/ramfan_ioctl.h"

#define RAMFAN_SERVICE_NAME   L"RAMFan"
#define RAMFAN_LOG_PATH       L"C:\\ProgramData\\RAMFan\\ramfan.log"

static SERVICE_STATUS         g_Status;
static SERVICE_STATUS_HANDLE  g_StatusHandle = NULL;
static HANDLE                 g_StopEvent = NULL;
static volatile LONG          g_InstallDisabled = 1;
/* g_FeedDisabled 已删除：§5.2 第 3 步批准后 --once 写回启用；常驻 0.5s 喂值阶段
   （需再批准）再引入独立的 SCM 生命周期开关 */
/* ---- 日志（服务模式写文件；--once 同时输出 stdout） ---- */
static void
LogMessage(const char *fmt, ...)
{
    va_list args;
    char buf[1024];
    SYSTEMTIME st;
    FILE *f;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    GetLocalTime(&st);
    /* 首次写日志时确保目录存在（服务以 SYSTEM 运行） */
    CreateDirectoryW(L"C:\\ProgramData\\RAMFan", NULL);
    f = _wfopen(RAMFAN_LOG_PATH, L"a");
    if (f != NULL) {
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
    printf("%s\n", buf);
}

/* ---- 打开驱动设备 ---- */
static HANDLE
OpenDevice(void)
{
    return CreateFileW(RAMFAN_WIN32_DEVICE,
                       GENERIC_READ | GENERIC_WRITE,
                       0,                    /* 独占，驱动串行队列 */
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

/* ---- 身份门禁检查（阶段 §5.2 第 1 步）：只读 QUERY_HW ---- */
/* 返回 0=身份匹配 1=硬件/IO 失败或身份不匹配 2=参数错误。不访问 SMBus、不写 NCT。 */
static int
RunIdentityGateCheck(void)
{
    HANDLE h;
    DWORD bytesReturned = 0;
    RAMFAN_QUERY_HW_OUT qh = {0};
    BOOL ok;

    h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        LogMessage("ERROR: 打开设备失败 GLE=%lu（驱动未加载？）", GetLastError());
        return 1;
    }

    ok = DeviceIoControl(h, IOCTL_RAMFAN_QUERY_HW, NULL, 0,
                         &qh, sizeof(qh), &bytesReturned, NULL);
    if (!ok || bytesReturned != sizeof(qh)) {
        LogMessage("ERROR: QUERY_HW 失败 GLE=%lu bytes=%lu",
                   GetLastError(), bytesReturned);
        CloseHandle(h);
        return 1;
    }
    LogMessage("QUERY_HW: SMBusBase=0x%04x ChipId=%02x%02x "
               "ControllerFound=%u ChipIdValid=%u HwMatched=%u",
               qh.SmbusBase, qh.ChipIdHi, qh.ChipIdLo,
               qh.ControllerFound, qh.ChipIdValid, qh.HwMatched);

    CloseHandle(h);

    if (!qh.HwMatched) {
        LogMessage("ERROR: 身份门禁未通过（预期 PCI DEV_790B + chip id %02x%02x），拒绝继续",
                   NCT_EXPECTED_CHIP_ID_HI, NCT_EXPECTED_CHIP_ID_LO);
        return 1;
    }
    LogMessage("身份门禁通过。");
    return 0;
}

/* ---- 实验观测：调用 READ_DIMM_TEMP 打印每槽状态（§5.2 第 2 步） ---- */
/* 返回 0=成功（至少一个槽返回数据） 1=失败/身份不匹配 2=参数错误。不写 NCT。 */
static int
RunReadDimm(void)
{
    HANDLE h;
    DWORD bytesReturned = 0;
    RAMFAN_READ_DIMM_OUT rd = {0};
    BOOL ok;
    int i;

    h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        LogMessage("ERROR: 打开设备失败 GLE=%lu（驱动未加载？）", GetLastError());
        return 1;
    }

    ok = DeviceIoControl(h, IOCTL_RAMFAN_READ_DIMM_TEMP, NULL, 0,
                         &rd, sizeof(rd), &bytesReturned, NULL);
    if (!ok || bytesReturned != sizeof(rd)) {
        DWORD gle = GetLastError();
        if (gle == ERROR_ACCESS_DENIED) {
            LogMessage("ERROR: READ_DIMM_TEMP 被拒（身份门禁未通过或访问被拒）；先运行 --identity 确认 HwMatched=1");
        } else {
            LogMessage("ERROR: READ_DIMM_TEMP 失败 GLE=%lu bytes=%lu", gle, bytesReturned);
        }
        CloseHandle(h);
        return 1;
    }

    LogMessage("READ_DIMM_TEMP: Count=%u AnySuccess=%u MaxCelsius=%u",
               rd.Count, rd.AnySuccess, rd.MaxCelsius);
    for (i = 0; i < rd.Count && i < RAMFAN_SPD_ADDR_COUNT; i++) {
        const RAMFAN_DIMM_RESULT *s = &rd.Slots[i];
        LogMessage("  DIMM 0x%02x: status=%u raw=0x%04x temp=%uC hst=0x%02x",
                   s->Address, s->Status, s->Raw, s->Celsius, s->HstSts);
    }
    LogMessage("  状态: 0=OK 2=超时 3=总线错误/不确定 4=非法数据 5=未检查；"
               "0x04/0x06 含空槽 NACK 与 CRC，BUS_ERR 语义由实验分析判定");

    CloseHandle(h);
    return rd.AnySuccess ? 0 : 1;
}



/* ---- 写回：--once 执行一次完整喂值（§5.2 第 3 步已批准） ---- */
static int
RunFeedOnce(void)
{
    HANDLE h;
    DWORD bytesReturned = 0;
    RAMFAN_FEED_ONCE_OUT feed = {0};
    BOOL ok;
    int i;

    h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        LogMessage("ERROR: 打开设备失败 GLE=%lu（驱动未加载？）", GetLastError());
        return 1;
    }

    /* 资源门禁必须由驱动处理；这里不先调用 QUERY_HW，避免先访问端口。 */
    ok = DeviceIoControl(h, IOCTL_RAMFAN_FEED_ONCE, NULL, 0,
                         &feed, sizeof(feed), &bytesReturned, NULL);
    if (!ok || bytesReturned != sizeof(feed)) {
        LogMessage("ERROR: FEED_ONCE 失败 GLE=%lu bytes=%lu",
                   GetLastError(), bytesReturned);
        CloseHandle(h);
        return 1;
    }

    if (feed.Status != RAMFAN_FEED_HW_MISMATCH) {
        for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
            const RAMFAN_DIMM_RESULT *slot = &feed.Slots[i];
            LogMessage("  DIMM 0x%02x: status=%u raw=0x%04x temp=%u°C hst=0x%02x",
                       slot->Address, slot->Status, slot->Raw, slot->Celsius,
                       slot->HstSts);
        }
    }
    LogMessage("FEED_ONCE: status=%u max=%u°C written=%u°C readback=%u°C",
               feed.Status, feed.MaxCelsius, feed.WrittenCelsius,
               feed.ReadBackCelsius);
    LogMessage("  状态: 0=OK(写回并读回一致) 1=读取失败未写 2=写入失败/读回不一致 "
               "3=硬件不匹配");

    CloseHandle(h);
    return feed.Status == RAMFAN_FEED_OK ? 0 : 1;
}


/* ---- SCM ---- */
static DWORD WINAPI
ServiceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(context);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_Status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_Status);
        if (g_StopEvent != NULL) {
            SetEvent(g_StopEvent);
        }
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static VOID WINAPI
ServiceMain(DWORD argc, LPWSTR *argv)
{
    int readStatus = 1;
    int attempt;
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StatusHandle = RegisterServiceCtrlHandlerExW(
        RAMFAN_SERVICE_NAME, ServiceCtrlHandler, NULL);
    if (g_StatusHandle == NULL) {
        return;
    }

    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_Status.dwWin32ExitCode = NO_ERROR;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 5000;
    SetServiceStatus(g_StatusHandle, &g_Status);

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_StopEvent == NULL) {
        g_Status.dwCurrentState = SERVICE_STOPPED;
        g_Status.dwWin32ExitCode = ERROR_NOT_ENOUGH_MEMORY;
        SetServiceStatus(g_StatusHandle, &g_Status);
        return;
    }

    /* 启动时执行只读身份门禁检查；失败有限重试。当前 SCM 服务不做常驻喂值循环，
       常驻 0.5s 喂值留后续阶段（需再批准）；单次写回用 --once 控制台模式验证。 */
    LogMessage("SERVICE START (identity gate check)");
    for (attempt = 0; attempt < 3; attempt++) {
        readStatus = RunIdentityGateCheck();
        if (readStatus == 0 || g_StopEvent == NULL ||
            WaitForSingleObject(g_StopEvent, 1000) == WAIT_OBJECT_0) {
            break;
        }
        LogMessage("WARN: 身份门禁检查失败，准备第 %d 次重试", attempt + 2);
    }
    if (readStatus != 0) {
        LogMessage("ERROR: 身份门禁检查重试仍失败，服务停止");
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
        g_Status.dwCurrentState = SERVICE_STOPPED;
        g_Status.dwWin32ExitCode = ERROR_DEVICE_NOT_CONNECTED;
        SetServiceStatus(g_StatusHandle, &g_Status);
        return;
    }
    g_Status.dwCurrentState = SERVICE_RUNNING;
    g_Status.dwCheckPoint = 0;
    SetServiceStatus(g_StatusHandle, &g_Status);

    /* 等待停止（后续阶段改为 0.5s 喂值循环） */
    WaitForSingleObject(g_StopEvent, INFINITE);

    LogMessage("SERVICE STOP");
    g_Status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_Status);
}

/* ---- 安装/卸载 ---- */
static int
InstallService(void)
{
    if (g_InstallDisabled) {
        printf("SCM 用户态服务安装当前禁用（SCM 生命周期未批准；本阶段用驱动直接加载 + --identity/--dimm/--once 验证）。\n");
    }
    SC_HANDLE scm, svc;
    WCHAR path[MAX_PATH];
    SERVICE_DESCRIPTION desc;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        printf("OpenSCManager 失败 GLE=%lu（需要管理员）\n", GetLastError());
        return 1;
    }
    svc = CreateServiceW(scm, RAMFAN_SERVICE_NAME,
                         L"RAMFan VirtualTEMP Feeder",
                         SERVICE_ALL_ACCESS,
                         SERVICE_WIN32_OWN_PROCESS,
                         SERVICE_DEMAND_START,   /* 开发测试；验收后才改自动 */
                         SERVICE_ERROR_NORMAL,
                         path, NULL, NULL, NULL, NULL, NULL);
    if (svc == NULL && GetLastError() != ERROR_SERVICE_EXISTS) {
        printf("CreateService 失败 GLE=%lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    if (svc == NULL) {
        svc = OpenServiceW(scm, RAMFAN_SERVICE_NAME, SERVICE_ALL_ACCESS);
    }
    if (svc != NULL) {
        desc.lpDescription = (LPWSTR)L"RAMFan VirtualTEMP Feeder（受控试验；§5.2 第 3 步单次写回已批准）";
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    printf("服务已安装（DEMAND_START）。启动：sc start RAMFan\n");
    return 0;
}

static int
UninstallService(void)
{
    if (g_InstallDisabled) {
        printf("SCM 用户态服务卸载当前禁用。\n");
        return 1;
    }
    SC_HANDLE scm, svc;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        printf("OpenSCManager 失败 GLE=%lu\n", GetLastError());
        return 1;
    }
    svc = OpenServiceW(scm, RAMFAN_SERVICE_NAME, DELETE);
    if (svc == NULL) {
        printf("OpenService 失败 GLE=%lu（服务未安装？）\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    if (!DeleteService(svc)) {
        printf("DeleteService 失败 GLE=%lu（先 sc stop RAMFan）\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    printf("服务已删除。\n");
    return 0;
}

/* 当前 PnP 骨架不允许用户态 SCM 安装/卸载入口修改系统。 */
/* ---- main ---- */
int
main(int argc, char **argv)
{
    int once = 0;
    int identity = 0;
    int dimm = 0;
    int install = 0;
    int uninstall = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--identity") == 0) {
            identity = 1;
        } else if (strcmp(argv[i], "--dimm") == 0) {
            dimm = 1;
        } else if (strcmp(argv[i], "--install") == 0) {
            install = 1;
        } else if (strcmp(argv[i], "--uninstall") == 0) {
            uninstall = 1;
        } else {
            printf("未知参数: %s\n", argv[i]);
            printf("用法: ramfan-service [--identity|--dimm|--once|--install|--uninstall]\n");
            return 2; /* 参数错误 */
        }
    }

    if (once + identity + dimm + install + uninstall > 1) {
        printf("参数互斥：--identity、--dimm、--once、--install、--uninstall 只能选择一个。\n");
        return 2;
    }

    if (install) {
        return InstallService();
    }
    if (uninstall) {
        return UninstallService();
    }
    if (identity) {
        return RunIdentityGateCheck();
    }
    if (dimm) {
        return RunReadDimm();
    }
    if (once) {
        return RunFeedOnce();
    }

    /* 默认：作为 SCM 服务运行 */
    {
        SERVICE_TABLE_ENTRYW table[] = {
            { RAMFAN_SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
            { NULL, NULL }
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            printf("StartServiceCtrlDispatcher 失败 GLE=%lu\n", GetLastError());
            return 1;
        }
    }
    return 0;
}
