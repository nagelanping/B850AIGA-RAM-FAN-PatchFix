/* ramfan-service.c — B850AIGA RAM-FAN Virtual_TEMP 补丁服务（阶段 2）
 *
 *   - --once、--install、--uninstall 当前拒绝；默认 SCM 模式仅执行阶段 2只读检查
 *   - 常驻 0.5s 喂值留到阶段 3，不接触裸端口
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
static volatile LONG          g_FeedDisabled = 1;

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

/* ---- 硬件识别 + 只读温度，返回 0=成功 1=硬件/IO 失败 ---- */
static int
RunReadOnlyCycle(void)
{
    HANDLE h;
    DWORD bytesReturned = 0;
    RAMFAN_QUERY_HW_OUT qh = {0};
    RAMFAN_READ_DIMM_OUT rd = {0};
    BOOL ok;
    int i;

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
    LogMessage("QUERY_HW: SMBusBase=0x%04x ChipId=%02x%02x HwMatched=%u",
               qh.SmbusBase, qh.ChipIdHi, qh.ChipIdLo, qh.HwMatched);
    if (!qh.HwMatched) {
        LogMessage("ERROR: 硬件不匹配（预期 SMBus 有效 + chip id %02x%02x），拒绝继续",
                   NCT_EXPECTED_CHIP_ID_HI, NCT_EXPECTED_CHIP_ID_LO);
        CloseHandle(h);
        return 1;
    }

    bytesReturned = 0;
    ok = DeviceIoControl(h, IOCTL_RAMFAN_READ_DIMM_TEMP, NULL, 0,
                         &rd, sizeof(rd), &bytesReturned, NULL);
    if (!ok || bytesReturned != sizeof(rd)) {
        LogMessage("ERROR: READ_DIMM_TEMP 失败 GLE=%lu bytes=%lu",
                   GetLastError(), bytesReturned);
        CloseHandle(h);
        return 1;
    }

    for (i = 0; i < rd.Count && i < RAMFAN_SPD_ADDR_COUNT; i++) {
        const RAMFAN_DIMM_RESULT *s = &rd.Slots[i];
        LogMessage("  DIMM 0x%02x: status=%u raw=0x%04x temp=%u°C",
                   s->Address, s->Status, s->Raw, s->Celsius);
    }
    LogMessage("READ_DIMM_TEMP: AnySuccess=%u MaxCelsius=%u",
               rd.AnySuccess, rd.MaxCelsius);

    CloseHandle(h);
    return rd.AnySuccess ? 0 : 1;
}

/* ---- 阶段 2：--once 执行一次完整喂值 ---- */
static int
RunFeedOnce(void)
{
    if (g_FeedDisabled) {
        LogMessage("当前资源识别骨架禁止 --once；未连接驱动。");
        return 1;
    }
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

    for (i = 0; i < RAMFAN_SPD_ADDR_COUNT; i++) {
        const RAMFAN_DIMM_RESULT *slot = &feed.Slots[i];
        LogMessage("  DIMM 0x%02x: status=%u raw=0x%04x temp=%u°C",
                   slot->Address, slot->Status, slot->Raw, slot->Celsius);
    }
    LogMessage("FEED_ONCE: status=%u max=%u°C written=%u°C readback=%u°C",
               feed.Status, feed.MaxCelsius, feed.WrittenCelsius,
               feed.ReadBackCelsius);

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

    /* 阶段 2：启动时只做只读检查；失败时有限重试，不执行 FEED_ONCE。 */
    LogMessage("SERVICE START (stage 2 read-only check)");
    for (attempt = 0; attempt < 3; attempt++) {
        readStatus = RunReadOnlyCycle();
        if (readStatus == 0 || g_StopEvent == NULL ||
            WaitForSingleObject(g_StopEvent, 1000) == WAIT_OBJECT_0) {
            break;
        }
        LogMessage("WARN: 只读检查失败，准备第 %d 次重试", attempt + 2);
    }
    if (readStatus != 0) {
        LogMessage("ERROR: 只读检查重试仍失败，服务停止");
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

    /* 等待停止（阶段 3 改为 0.5s 喂值循环） */
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
        printf("当前 PnP 资源识别骨架禁止用户态服务安装。\n");
        return 1;
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
        desc.lpDescription = (LPWSTR)L"RAMFan 阶段 2开发测试；FEED_ONCE 当前受端口资源门禁阻断";
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
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
        printf("当前 PnP 资源识别骨架禁止用户态服务卸载。\n");
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
    int install = 0;
    int uninstall = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--install") == 0) {
            install = 1;
        } else if (strcmp(argv[i], "--uninstall") == 0) {
            uninstall = 1;
        } else {
            printf("未知参数: %s\n", argv[i]);
            printf("用法: ramfan-service [--once|--install|--uninstall]\n");
            return 2; /* 参数错误 */
        }
    }

    if (once + install + uninstall > 1) {
        printf("参数互斥：--once、--install、--uninstall 只能选择一个。\n");
        return 2;
    }

    if (install) {
        return InstallService();
    }
    if (uninstall) {
        return UninstallService();
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
