/**
 * @file dsh_pwa_shim.cpp
 * @brief DSH Web 静默守护与生命周期联动 Shim (快捷方式参数驱动 + 直通 Node)
 *
 * 功能职责：
 * 1. 后台工作目录由快捷方式参数 (--cwd <dir>) 提供，0 硬编码。
 * 2. 直通 Node.js 启动：从 pnpm 的 dsh shim 读取 cmd-shim-target，跳过 PowerShell。
 * 3. 将 Node stdout/stderr 接到匿名管道（同时避免无窗口模式下的 stdout 崩溃）。
 * 4. 将后台进程绑定入 Windows Job Object（JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE）。
 * 5. 立刻显示原生启动窗（不打开无 token 的页面，避免 404/401）。
 * 6. 从 stdout 解析 `dsh web: http://127.0.0.1:<port>/?token=...`（0.1.2 起根路径强制认证）。
 * 7. token 就绪后只拉起一次 Edge --app=<tokenUrl>，用户第一眼是 DSH 自己的加载页。
 * 8. 捕获窗口 HWND/PID 并立即最大化；SetWinEventHook + MsgWaitForMultipleObjects 事件驱动等待。
 * 9. 窗口关闭时 TerminateJobObject，回收 node 进程树并释放端口。
 * 10. 每次启动清空 exe 同级 dsh_pwa_shim.log / dsh_web_console.log；运行中追加，分别 512KB / 2MB 防爆。token 入日志时脱敏。
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#endif

// ==========================================
// 常量与配置定义 (集中优于分散，SSOT 原则)
// ==========================================
namespace Config {
    constexpr const wchar_t* APP_WINDOW_CLASS = L"Chrome_WidgetWin_1";
    constexpr const wchar_t* SPLASH_CLASS    = L"DshPwaShimSplash";
    constexpr int SPLASH_WIDTH               = 440;
    constexpr int SPLASH_HEIGHT              = 240;
    constexpr const wchar_t* APP_TITLE_KW1   = L"DeepSeek";
    constexpr const wchar_t* APP_TITLE_KW2   = L"Harness";
    constexpr const wchar_t* APP_TITLE_KW3   = L"dsh";
    constexpr const wchar_t* APP_TITLE_KW4   = L"3080";

    constexpr const wchar_t* LOG_FILE_NAME   = L"dsh_pwa_shim.log";
    constexpr const wchar_t* CONSOLE_LOG_FILE_NAME = L"dsh_web_console.log";
    constexpr DWORD MAX_LOG_SIZE_BYTES       = 512 * 1024;
    constexpr DWORD MAX_CONSOLE_LOG_SIZE_BYTES = 2 * 1024 * 1024;

    constexpr DWORD AUTH_URL_TIMEOUT_MS      = 60000;
    constexpr DWORD AUTH_URL_INTERVAL_MS     = 30;
    constexpr size_t STDOUT_ACC_CAP_BYTES    = 256 * 1024;

    constexpr DWORD APP_CAPTURE_TIMEOUT_MS   = 15000;
    constexpr DWORD APP_CAPTURE_INTERVAL_MS  = 30;

    constexpr const char* AUTH_URL_MARKER    = "dsh web: ";
}

// ==========================================
// 全局状态 (事件钩子回调与主线程通信)
// ==========================================
namespace GlobalState {
    static HWND g_TargetHwnd = NULL;
    static DWORD g_TargetPid = 0;
    static bool g_AppClosed = false;
    static HANDLE g_ConsoleLog = NULL;
    static HANDLE g_DrainThread = NULL;
    static HWND g_SplashHwnd = NULL;
    static bool g_SplashCancelled = false;
    static wchar_t g_SplashStatus[160];
}

// ==========================================
// 符号声明 (报纸结构前置)
// ==========================================

/**
 * @brief 获取当前可执行文件所在目录绝对路径。
 * @return 目录绝对路径字符串。
 */
std::wstring GetExeDirectory();

/**
 * @brief 获取位于 exe 同级目录下的日志文件绝对路径。
 * @return 日志文件完整路径。
 */
std::wstring GetLogFilePath();

/**
 * @brief 解析当前的目标工作目录 (优先从快捷方式传递的 --cwd / -w 命令行参数读取，默认使用 CurrentDirectory)。
 * @return 目标工作目录绝对路径。
 */
std::wstring ResolveTargetWorkingDirectory();

/**
 * @brief 写入文件日志 (含磁盘防爆机制与时间戳)。
 * @param level 日志等级 (如 "INFO", "WARN", "ERROR")。
 * @param msg 日志信息内容。
 */
void LogMessage(const std::string& level, const std::string& msg);

/**
 * @brief 将宽字符字符串转换为 UTF-8 字符串以供日志记录。
 * @param wstr 宽字符串。
 * @return UTF-8 字符串。
 */
std::string ToUtf8(const std::wstring& wstr);

/**
 * @brief 将 UTF-8 字符串转换为宽字符串。
 * @param str UTF-8 字符串。
 * @return 宽字符串。
 */
std::wstring FromUtf8(const std::string& str);

/**
 * @brief 将 URL 中的 token 查询值替换为 ***，避免进程令牌落入日志。
 * @param text 可能含 token 的文本。
 * @return 脱敏后的文本。
 */
std::string RedactToken(const std::string& text);

/**
 * @brief 初始化 Windows Job Object (KillOnClose)。
 * @return 创建成功的 Job 句柄，若失败返回 NULL。
 */
HANDLE InitializeJobObject();

/**
 * @brief 从 pnpm 的 dsh shim 读取 cmd-shim-target，定位 node.exe / bin.js / NODE_PATH。
 * @param[out] outNodeExe node.exe 路径。
 * @param[out] outBinJs dsh bin.js 路径。
 * @param[out] outNodePath NODE_PATH 依赖目录。
 * @return 解析是否成功。
 */
bool ResolveDshDirectPaths(std::wstring& outNodeExe, std::wstring& outBinJs, std::wstring& outNodePath);

/**
 * @brief 启动后台 dsh web 服务 (直通 Node.js，stdout/stderr 接到管道)。
 * @param workingDir 目标工作目录。
 * @param[out] pi 存储创建出的进程与主线程句柄。
 * @param[out] hStdoutRead 父进程持有的管道读端。
 * @return 启动是否成功。
 */
bool LaunchBackendProcess(const std::wstring& workingDir, PROCESS_INFORMATION& pi, HANDLE& hStdoutRead);

/**
 * @brief 排空管道并等待 stdout 出现带 token 的 `dsh web:` URL。
 * @param hStdoutRead 后台进程 stdout 读端。
 * @param hProcess 后台进程句柄 (fail-fast：提前退出则失败)。
 * @param cancelHwnd 启动窗；用户关闭则立即失败。
 * @param timeoutMs 超时时间 (毫秒)。
 * @param acc 已排空的 stdout 累积缓冲，函数会继续向其中追加。
 * @param[out] outAuthUrl 解析到的认证 URL。
 * @return 是否在超时前拿到合法 token URL。
 */
bool WaitForAuthUrl(HANDLE hStdoutRead, HANDLE hProcess, HWND cancelHwnd, DWORD timeoutMs, std::string& acc, std::string& outAuthUrl);

/**
 * @brief 解析本机 Edge 可执行文件路径 (Program Files (x86) 优先，其次 Program Files)。
 * @param relative 相对 Microsoft\\Edge\\Application 的文件名。
 * @return 存在则返回绝对路径，否则空串。
 */
std::wstring ResolveEdgeFile(const wchar_t* relative);

/**
 * @brief 收集当前系统中所有现存匹配特征的窗口句柄。
 * @return 窗口 HWND 列表。
 */
std::vector<HWND> SnapshotExistingAppWindows();

/**
 * @brief 立刻显示原生启动窗。
 * @param hInstance 当前模块实例。
 * @return 启动窗 HWND，失败返回 NULL。
 */
HWND CreateSplashWindow(HINSTANCE hInstance);

/**
 * @brief 更新启动窗状态文案并重绘。
 * @param status 状态文案。
 */
void SetSplashStatus(const wchar_t* status);

/**
 * @brief 销毁原生启动窗。
 */
void DestroySplashWindow();

/**
 * @brief 用 msedge.exe --app=<tokenUrl> 打开独立应用窗口。
 * @param authUrl 带本次进程 token 的根 URL。
 * @return 唤起是否成功。
 */
bool LaunchAppWithAuthUrl(const std::wstring& authUrl);

/**
 * @brief 注册窗口销毁钩子。
 * @param targetHwnd 目标窗口。
 * @param targetPid 目标进程 PID。
 * @return 钩子句柄，失败则为 NULL。
 */
HWINEVENTHOOK InstallWindowDestroyHook(HWND targetHwnd, DWORD targetPid);

/**
 * @brief 捕获新弹出的应用窗口句柄与其宿主 PID (30ms 高频扫描)。
 * @param oldHwnds 启动前已存在的窗口句柄列表。
 * @param timeoutMs 查找超时时间 (毫秒)。
 * @param[out] outPid 输出目标窗口所属进程 PID。
 * @param hStdoutRead 后台 stdout 读端，捕获期间排空以免管道堵死 Node。
 * @param stdoutAcc stdout 累积缓冲。
 * @return 成功捕获到的应用窗口 HWND，超时则返回 NULL。
 */
HWND CaptureAppWindowWithPid(const std::vector<HWND>& oldHwnds, DWORD timeoutMs, DWORD& outPid, HANDLE hStdoutRead, std::string& stdoutAcc);

/**
 * @brief 将已捕获的应用窗口最大化（Chromium 可能忽略 --start-maximized）。
 * @param hwnd 目标窗口。
 */
void MaximizeAppWindow(HWND hwnd);

/**
 * @brief 纯事件驱动的主监控事件泵。
 * @param targetHwnd 目标应用窗口句柄。
 * @param targetPid 目标应用进程 PID。
 * @param hBackendProcess 后台服务进程句柄。
 */
void RunEventDrivenLifecyclePump(HWND targetHwnd, DWORD targetPid, HANDLE hBackendProcess);

/**
 * @brief 启动阻塞读线程，把剩余 stdout/stderr 写入 dsh_web_console.log。
 * @param hStdoutRead 管道读端所有权转入该线程。
 */
void StartConsoleDrainThread(HANDLE hStdoutRead);

/**
 * @brief 彻底终止并清理 Job Object 及后台进程树。
 * @param hJob 作业对象句柄。
 * @param hProcess 后台主进程句柄。
 * @param hStdoutRead 管道读端，可为 NULL。
 */
void CleanupBackendProcess(HANDLE hJob, HANDLE hProcess, HANDLE hStdoutRead);

/**
 * @brief 打开 exe 同级 dsh_web_console.log（每次启动覆盖）。
 * @return 可写句柄，失败返回 NULL。
 */
HANDLE OpenConsoleLog();

/**
 * @brief 清空 exe 同级 dsh_pwa_shim.log，供本次启动从头写。
 */
void TruncateShimLog();

/**
 * @brief 检查命令行中是否含有 --debug 参数。
 * @param lpCmdLine 宽字符命令行字符串。
 * @return 是否开启 debug 模式。
 */
bool IsDebugMode(LPCWSTR lpCmdLine);

// ==========================================
// 报纸结构：主流程入口 (wWinMain)
// ==========================================

/**
 * @brief Win32 GUI 入口点（MinGW -municode 要求 wWinMain），彻底免去控制台黑框。
 */
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)nCmdShow;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    bool debug = IsDebugMode(lpCmdLine);
    TruncateShimLog();
    GlobalState::g_ConsoleLog = OpenConsoleLog();
    if (GlobalState::g_ConsoleLog == NULL) {
        LogMessage("WARN", "无法创建 dsh_web_console.log，dsh web 输出不会落盘");
    }
    LogMessage("INFO", "========== DSH Desktop Shim 启动 ==========");
    if (debug) {
        LogMessage("DEBUG", "已启用调试模式");
    }

    std::wstring workingDir = ResolveTargetWorkingDirectory();
    LogMessage("INFO", "目标后台工作目录 (来自快捷方式参数): " + ToUtf8(workingDir));

    HWND splash = CreateSplashWindow(hInstance);
    if (splash == NULL) {
        LogMessage("ERROR", "创建启动窗失败");
        CoUninitialize();
        return 5;
    }
    SetSplashStatus(L"正在启动 DeepSeek Harness ...");

    HANDLE hJob = InitializeJobObject();
    if (!hJob) {
        LogMessage("ERROR", "Job Object 创建或配置失败，LastError: " + std::to_string(GetLastError()));
        if (debug) MessageBoxW(NULL, L"Job Object 创建失败", L"DSH Shim 错误", MB_ICONERROR);
        DestroySplashWindow();
        CoUninitialize();
        return 2;
    }
    LogMessage("INFO", "严格隔离 Job Object 配置完成");

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    HANDLE hStdoutRead = NULL;
    if (!LaunchBackendProcess(workingDir, pi, hStdoutRead)) {
        LogMessage("ERROR", "LaunchBackendProcess 启动失败，LastError: " + std::to_string(GetLastError()));
        if (debug) MessageBoxW(NULL, L"后台 dsh web 服务启动失败，请检查 Node/dsh 安装与日志", L"DSH Shim 错误", MB_ICONERROR);
        CloseHandle(hJob);
        DestroySplashWindow();
        CoUninitialize();
        return 3;
    }
    LogMessage("INFO", "后台服务进程已创建 (PID: " + std::to_string(pi.dwProcessId) + ")");

    if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
        LogMessage("ERROR", "AssignProcessToJobObject 绑定失败，LastError: " + std::to_string(GetLastError()));
    } else {
        LogMessage("INFO", "已成功将后台服务进程绑定至 Job Object 树");
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    SetSplashStatus(L"正在等待认证地址 ...");
    LogMessage("INFO", "正在等待 stdout 出现 dsh web 认证 URL ...");
    std::string stdoutAcc;
    std::string authUrlUtf8;
    if (!WaitForAuthUrl(hStdoutRead, pi.hProcess, splash, Config::AUTH_URL_TIMEOUT_MS, stdoutAcc, authUrlUtf8)) {
        if (GlobalState::g_SplashCancelled) {
            LogMessage("INFO", "启动窗已关闭，回收后台");
        } else {
            LogMessage("ERROR", "未能在超时时间内解析到 dsh web 认证 URL");
            if (debug) MessageBoxW(NULL, L"未能解析 dsh web 认证 URL，请查看日志", L"DSH Shim 错误", MB_ICONERROR);
        }
        CleanupBackendProcess(hJob, pi.hProcess, hStdoutRead);
        DestroySplashWindow();
        CoUninitialize();
        return GlobalState::g_SplashCancelled ? 0 : 4;
    }
    LogMessage("INFO", "已捕获认证 URL: " + RedactToken(authUrlUtf8));

    std::vector<HWND> oldHwnds = SnapshotExistingAppWindows();
    LogMessage("INFO", "启动前匹配窗口快照数量: " + std::to_string(oldHwnds.size()));
    SetSplashStatus(L"正在打开界面 ...");
    if (!LaunchAppWithAuthUrl(FromUtf8(authUrlUtf8))) {
        LogMessage("ERROR", "唤起 Edge --app 窗口失败");
        if (debug) MessageBoxW(NULL, L"唤起 Edge 应用窗口失败", L"DSH Shim 错误", MB_ICONERROR);
        CleanupBackendProcess(hJob, pi.hProcess, hStdoutRead);
        DestroySplashWindow();
        CoUninitialize();
        return 5;
    }
    LogMessage("INFO", "已用认证 URL 唤起 Edge --app 窗口");

    DWORD targetPid = 0;
    HWND targetHwnd = CaptureAppWindowWithPid(oldHwnds, Config::APP_CAPTURE_TIMEOUT_MS, targetPid, hStdoutRead, stdoutAcc);
    GlobalState::g_TargetHwnd = targetHwnd;
    GlobalState::g_TargetPid = targetPid;
    GlobalState::g_AppClosed = false;
    if (targetHwnd != NULL) {
        LogMessage("INFO", "已锁定目标窗口 HWND: " + std::to_string(reinterpret_cast<uintptr_t>(targetHwnd)) + ", PID: " + std::to_string(targetPid));
        MaximizeAppWindow(targetHwnd);
    } else {
        LogMessage("WARN", "未能精确锁定特定 HWND，启用全局窗口销毁事件监听");
    }

    DestroySplashWindow();
    StartConsoleDrainThread(hStdoutRead);
    hStdoutRead = NULL;
    RunEventDrivenLifecyclePump(targetHwnd, targetPid, pi.hProcess);

    LogMessage("INFO", "窗口已销毁，正在强制清理后台进程树 ...");
    CleanupBackendProcess(hJob, pi.hProcess, hStdoutRead);
    CoUninitialize();
    LogMessage("INFO", "========== DSH Desktop Shim 正常退出 (后台已完全回收) ==========");

    return 0;
}

// ==========================================
// 详细功能与辅助函数实现
// ==========================================

std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH];
    ZeroMemory(path, sizeof(path));
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

std::wstring GetLogFilePath() {
    static std::wstring s_logPath = GetExeDirectory() + L"\\" + Config::LOG_FILE_NAME;
    return s_logPath;
}

std::wstring ResolveTargetWorkingDirectory() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != NULL) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--cwd") == 0 || wcscmp(argv[i], L"-w") == 0) {
                if (i + 1 < argc) {
                    std::wstring customDir(argv[i + 1]);
                    LocalFree(argv);
                    return customDir;
                }
            }
        }
        LocalFree(argv);
    }

    wchar_t curDir[MAX_PATH];
    ZeroMemory(curDir, sizeof(curDir));
    GetCurrentDirectoryW(MAX_PATH, curDir);
    return std::wstring(curDir);
}

std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), NULL, 0, NULL, NULL);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &str[0], sizeNeeded, NULL, NULL);
    return str;
}

std::wstring FromUtf8(const std::string& str) {
    if (str.empty()) return std::wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), NULL, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &wstr[0], sizeNeeded);
    return wstr;
}

std::string RedactToken(const std::string& text) {
    std::string out = text;
    const std::string key = "token=";
    size_t pos = 0;
    while ((pos = out.find(key, pos)) != std::string::npos) {
        size_t start = pos + key.size();
        size_t end = out.find_first_of(" \t\r\n&", start);
        if (end == std::string::npos) end = out.size();
        out.replace(start, end - start, "***");
        pos = start + 3;
    }
    return out;
}

void TruncateShimLog() {
    HANDLE hFile = CreateFileW(
        GetLogFilePath().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
    }
}

void LogMessage(const std::string& level, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    char timeBuf[64];
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string line = "[" + std::string(timeBuf) + "] [" + level + "] " + msg + "\r\n";

    std::wstring logFilePath = GetLogFilePath();
    HANDLE hFile = CreateFileW(
        logFilePath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fileSize;
        if (GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > Config::MAX_LOG_SIZE_BYTES) {
            SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
            SetEndOfFile(hFile);
        } else {
            SetFilePointer(hFile, 0, NULL, FILE_END);
        }

        DWORD bytesWritten = 0;
        WriteFile(hFile, line.c_str(), static_cast<DWORD>(line.length()), &bytesWritten, NULL);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }
}

HANDLE OpenConsoleLog() {
    std::wstring path = GetExeDirectory() + L"\\" + Config::CONSOLE_LOG_FILE_NAME;
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    return hFile;
}

static void WriteConsoleLog(const char* data, DWORD size) {
    HANDLE hFile = GlobalState::g_ConsoleLog;
    if (hFile == NULL || data == NULL || size == 0) return;

    LARGE_INTEGER fileSize;
    if (GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > Config::MAX_CONSOLE_LOG_SIZE_BYTES) {
        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        SetEndOfFile(hFile);
    }

    DWORD written = 0;
    WriteFile(hFile, data, size, &written, NULL);
}

bool IsDebugMode(LPCWSTR lpCmdLine) {
    if (lpCmdLine == NULL) return false;
    if (wcsstr(lpCmdLine, L"--debug") != NULL) return true;
    const wchar_t* p = lpCmdLine;
    while (*p != 0) {
        while (*p == L' ') ++p;
        if (p[0] == L'-' && p[1] == L'd' && (p[2] == 0 || p[2] == L' ')) return true;
        while (*p != 0 && *p != L' ') ++p;
    }
    return false;
}

HANDLE InitializeJobObject() {
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (!hJob) return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    ZeroMemory(&jeli, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        CloseHandle(hJob);
        return NULL;
    }
    return hJob;
}

static bool ReadEntireFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    out.assign(static_cast<size_t>(size.QuadPart), 0);
    DWORD read = 0;
    BOOL ok = ReadFile(hFile, &out[0], static_cast<DWORD>(out.size()), &read, NULL);
    CloseHandle(hFile);
    if (!ok) return false;
    out.resize(read);
    return true;
}

static bool ParseCmdShimTarget(const std::string& text, std::wstring& outPath) {
    const std::string key = "cmd-shim-target=";
    size_t pos = text.rfind(key);
    if (pos == std::string::npos) return false;
    size_t start = pos + key.size();
    size_t end = text.find_first_of("\r\n", start);
    if (end == std::string::npos) end = text.size();
    std::string raw = text.substr(start, end - start);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
    if (raw.empty()) return false;
    for (char& ch : raw) {
        if (ch == '/') ch = '\\';
    }
    outPath = FromUtf8(raw);
    return true;
}

static std::wstring ParentPath(const std::wstring& path) {
    wchar_t buf[MAX_PATH];
    ZeroMemory(buf, sizeof(buf));
    lstrcpynW(buf, path.c_str(), MAX_PATH);
    PathRemoveFileSpecW(buf);
    return std::wstring(buf);
}

bool ResolveDshDirectPaths(std::wstring& outNodeExe, std::wstring& outBinJs, std::wstring& outNodePath) {
    wchar_t localApp[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) == 0) {
        LogMessage("ERROR", "读取 LOCALAPPDATA 失败");
        return false;
    }

    std::wstring pnpmBin = std::wstring(localApp) + L"\\pnpm\\bin";
    std::wstring pnpmNode = pnpmBin + L"\\node.exe";
    if (!PathFileExistsW(pnpmNode.c_str())) {
        LogMessage("ERROR", "未找到 pnpm 自带 node.exe: " + ToUtf8(pnpmNode));
        return false;
    }
    outNodeExe = pnpmNode;

    std::wstring shimPath = pnpmBin + L"\\dsh";
    if (!PathFileExistsW(shimPath.c_str())) {
        LogMessage("ERROR", "未找到 pnpm dsh shim: " + ToUtf8(shimPath));
        return false;
    }

    std::string shimText;
    if (!ReadEntireFileUtf8(shimPath, shimText)) {
        LogMessage("ERROR", "读取 pnpm dsh shim 失败: " + ToUtf8(shimPath));
        return false;
    }

    if (!ParseCmdShimTarget(shimText, outBinJs)) {
        LogMessage("ERROR", "pnpm dsh shim 缺少 cmd-shim-target，无法定位 bin.js");
        return false;
    }
    if (!PathFileExistsW(outBinJs.c_str())) {
        LogMessage("ERROR", "cmd-shim-target 指向的 bin.js 不存在: " + ToUtf8(outBinJs));
        return false;
    }

    // {root}/node_modules/@deepseek-ai/dsh/lib/bin.js → {root}/node_modules/.pnpm/node_modules
    std::wstring root = outBinJs;
    for (int i = 0; i < 5; ++i) {
        root = ParentPath(root);
    }
    outNodePath = root + L"\\node_modules\\.pnpm\\node_modules";
    if (!PathFileExistsW(outNodePath.c_str())) {
        LogMessage("ERROR", "NODE_PATH 目录不存在: " + ToUtf8(outNodePath));
        return false;
    }

    LogMessage("INFO", "node.exe: " + ToUtf8(outNodeExe));
    LogMessage("INFO", "bin.js: " + ToUtf8(outBinJs));
    LogMessage("INFO", "NODE_PATH: " + ToUtf8(outNodePath));
    return true;
}

bool LaunchBackendProcess(const std::wstring& workingDir, PROCESS_INFORMATION& pi, HANDLE& hStdoutRead) {
    hStdoutRead = NULL;

    std::wstring nodeExe, binJs, nodePath;
    if (!ResolveDshDirectPaths(nodeExe, binJs, nodePath)) {
        return false;
    }

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hNul = CreateFileW(
        L"NUL",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        0,
        NULL
    );
    if (hNul == INVALID_HANDLE_VALUE) {
        LogMessage("ERROR", "打开 NUL 失败，LastError: " + std::to_string(GetLastError()));
        return false;
    }

    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        LogMessage("ERROR", "CreatePipe 失败，LastError: " + std::to_string(GetLastError()));
        CloseHandle(hNul);
        return false;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hNul;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    SetEnvironmentVariableW(L"NODE_PATH", nodePath.c_str());

    std::wstring cmdLine = L"\"" + nodeExe + L"\" --expose-internals \"" + binJs + L"\" web --no-open";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL success = CreateProcessW(
        NULL,
        cmdBuf.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        NULL,
        workingDir.c_str(),
        &si,
        &pi
    );

    CloseHandle(hWrite);
    CloseHandle(hNul);

    if (!success) {
        LogMessage("ERROR", "CreateProcessW(node) 失败，LastError: " + std::to_string(GetLastError()));
        CloseHandle(hRead);
        return false;
    }

    hStdoutRead = hRead;
    return true;
}

static bool DrainStdout(HANDLE hRead, std::string& acc) {
    DWORD avail = 0;
    if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL)) {
        return false;
    }
    if (avail == 0) return true;

    std::string chunk(avail, 0);
    DWORD read = 0;
    if (!ReadFile(hRead, &chunk[0], avail, &read, NULL)) {
        return false;
    }
    chunk.resize(read);
    WriteConsoleLog(chunk.data(), read);
    acc.append(chunk);
    if (acc.size() > Config::STDOUT_ACC_CAP_BYTES) {
        acc.erase(0, acc.size() - Config::STDOUT_ACC_CAP_BYTES / 2);
    }
    return true;
}

static bool ExtractAuthUrl(const std::string& acc, std::string& outUrl) {
    size_t pos = 0;
    const std::string marker = Config::AUTH_URL_MARKER;
    while ((pos = acc.find(marker, pos)) != std::string::npos) {
        size_t start = pos + marker.size();
        size_t end = acc.find_first_of(" \t\r\n", start);
        if (end == std::string::npos) {
            return false;
        }
        std::string url = acc.substr(start, end - start);
        if (url.compare(0, 7, "http://") == 0 && url.find("?token=") != std::string::npos) {
            outUrl = url;
            return true;
        }
        pos = end;
    }
    return false;
}

static void PumpPendingMessages() {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            GlobalState::g_AppClosed = true;
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool WaitForAuthUrl(HANDLE hStdoutRead, HANDLE hProcess, HWND cancelHwnd, DWORD timeoutMs, std::string& acc, std::string& outAuthUrl) {
    DWORD startTick = GetTickCount();

    while ((GetTickCount() - startTick) < timeoutMs) {
        PumpPendingMessages();
        if (GlobalState::g_SplashCancelled) {
            LogMessage("INFO", "等待认证 URL 时启动窗已关闭");
            return false;
        }
        if (cancelHwnd != NULL && !IsWindow(cancelHwnd)) {
            LogMessage("INFO", "等待认证 URL 时启动窗已无效");
            GlobalState::g_SplashCancelled = true;
            return false;
        }

        if (hProcess != NULL) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                DrainStdout(hStdoutRead, acc);
                LogMessage("ERROR", "后台服务进程已提前退出，ExitCode: " + std::to_string(exitCode));
                if (!acc.empty()) {
                    LogMessage("ERROR", "进程输出尾部: " + RedactToken(acc.substr(acc.size() > 2048 ? acc.size() - 2048 : 0)));
                }
                return false;
            }
        }

        if (!DrainStdout(hStdoutRead, acc)) {
            LogMessage("ERROR", "读取后台 stdout 管道失败，LastError: " + std::to_string(GetLastError()));
            return false;
        }
        if (ExtractAuthUrl(acc, outAuthUrl)) {
            return true;
        }

        Sleep(Config::AUTH_URL_INTERVAL_MS);
    }

    DrainStdout(hStdoutRead, acc);
    LogMessage("ERROR", "等待认证 URL 超时");
    if (!acc.empty()) {
        LogMessage("ERROR", "进程输出尾部: " + RedactToken(acc.substr(acc.size() > 2048 ? acc.size() - 2048 : 0)));
    } else {
        LogMessage("ERROR", "stdout 为空：dsh web 可能尚未打印 URL，或输出未进入管道");
    }
    return false;
}

std::wstring ResolveEdgeFile(const wchar_t* relative) {
    const wchar_t* envNames[] = { L"ProgramFiles(x86)", L"ProgramFiles" };
    for (size_t i = 0; i < 2; ++i) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(envNames[i], buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        std::wstring cand = std::wstring(buf) + L"\\Microsoft\\Edge\\Application\\" + relative;
        if (PathFileExistsW(cand.c_str())) {
            return cand;
        }
    }
    return std::wstring();
}

static bool SpawnEdge(const std::wstring& exe, const std::wstring& cmdLine) {
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMAXIMIZED;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL success = CreateProcessW(
        exe.c_str(),
        cmdBuf.data(),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );
    if (!success) {
        LogMessage("ERROR", "CreateProcessW(" + ToUtf8(exe) + ") 失败，LastError: " + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct WindowSearchContext {
    std::vector<HWND> results;
};

static bool MatchesAppFeatures(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) {
        return false;
    }

    wchar_t className[256];
    ZeroMemory(className, sizeof(className));
    GetClassNameW(hwnd, className, 256);

    if (wcscmp(className, Config::APP_WINDOW_CLASS) == 0) {
        return true;
    }
    return false;
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    if (MatchesAppFeatures(hwnd)) {
        WindowSearchContext* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
        ctx->results.push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> SnapshotExistingAppWindows() {
    WindowSearchContext ctx;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));
    return ctx.results;
}

static LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            RECT titleRc = rc;
            titleRc.top += 56;
            titleRc.bottom = titleRc.top + 40;
            DrawTextW(hdc, L"DeepSeek Harness", -1, &titleRc, DT_CENTER | DT_SINGLELINE);

            SetTextColor(hdc, RGB(170, 170, 170));
            RECT statusRc = rc;
            statusRc.top = titleRc.bottom + 12;
            DrawTextW(hdc, GlobalState::g_SplashStatus, -1, &statusRc, DT_CENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            GlobalState::g_SplashCancelled = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (GlobalState::g_SplashHwnd == hwnd) GlobalState::g_SplashHwnd = NULL;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND CreateSplashWindow(HINSTANCE hInstance) {
    lstrcpynW(GlobalState::g_SplashStatus, L"正在启动 ...", 160);
    GlobalState::g_SplashCancelled = false;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = SplashWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = Config::SPLASH_CLASS;
    RegisterClassW(&wc);

    int x = (GetSystemMetrics(SM_CXSCREEN) - Config::SPLASH_WIDTH) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - Config::SPLASH_HEIGHT) / 2;
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_TOPMOST,
        Config::SPLASH_CLASS,
        L"DeepSeek Harness",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, Config::SPLASH_WIDTH, Config::SPLASH_HEIGHT,
        NULL, NULL, hInstance, NULL
    );
    if (hwnd == NULL) return NULL;

    GlobalState::g_SplashHwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

void SetSplashStatus(const wchar_t* status) {
    lstrcpynW(GlobalState::g_SplashStatus, status ? status : L"", 160);
    if (GlobalState::g_SplashHwnd) {
        InvalidateRect(GlobalState::g_SplashHwnd, NULL, TRUE);
        UpdateWindow(GlobalState::g_SplashHwnd);
        PumpPendingMessages();
    }
}

void DestroySplashWindow() {
    if (GlobalState::g_SplashHwnd) {
        DestroyWindow(GlobalState::g_SplashHwnd);
        GlobalState::g_SplashHwnd = NULL;
        PumpPendingMessages();
    }
}

bool LaunchAppWithAuthUrl(const std::wstring& authUrl) {
    std::wstring edge = ResolveEdgeFile(L"msedge.exe");
    if (edge.empty()) {
        LogMessage("ERROR", "未找到 msedge.exe（Program Files / Program Files (x86)）");
        return false;
    }
    LogMessage("INFO", "Edge: " + ToUtf8(edge));
    std::wstring cmdLine = L"\"" + edge + L"\" --profile-directory=Default --start-maximized --app=\"" + authUrl + L"\"";
    return SpawnEdge(edge, cmdLine);
}

void MaximizeAppWindow(HWND hwnd) {
    if (hwnd == NULL || !IsWindow(hwnd)) {
        return;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    PostMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    LogMessage("INFO", "已请求最大化目标窗口");
}

HWND CaptureAppWindowWithPid(const std::vector<HWND>& oldHwnds, DWORD timeoutMs, DWORD& outPid, HANDLE hStdoutRead, std::string& stdoutAcc) {
    DWORD startTick = GetTickCount();

    while ((GetTickCount() - startTick) < timeoutMs) {
        DrainStdout(hStdoutRead, stdoutAcc);
        WindowSearchContext ctx;
        EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));

        for (HWND hwnd : ctx.results) {
            if (std::find(oldHwnds.begin(), oldHwnds.end(), hwnd) == oldHwnds.end()) {
                wchar_t title[512];
                ZeroMemory(title, sizeof(title));
                GetWindowTextW(hwnd, title, 512);

                if (wcslen(title) > 0) {
                    GetWindowThreadProcessId(hwnd, &outPid);
                    return hwnd;
                }
            }
        }
        Sleep(Config::APP_CAPTURE_INTERVAL_MS);
    }

    for (HWND hwnd : SnapshotExistingAppWindows()) {
        wchar_t title[512];
        ZeroMemory(title, sizeof(title));
        GetWindowTextW(hwnd, title, 512);
        std::wstring titleStr(title);
        if (titleStr.find(Config::APP_TITLE_KW1) != std::wstring::npos ||
            titleStr.find(Config::APP_TITLE_KW2) != std::wstring::npos ||
            titleStr.find(Config::APP_TITLE_KW3) != std::wstring::npos ||
            titleStr.find(Config::APP_TITLE_KW4) != std::wstring::npos) {
            GetWindowThreadProcessId(hwnd, &outPid);
            return hwnd;
        }
    }

    return NULL;
}

static VOID CALLBACK WinEventProcCallback(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime
) {
    (void)hWinEventHook;
    (void)idChild;
    (void)idEventThread;
    (void)dwmsEventTime;

    if (event == EVENT_OBJECT_DESTROY && idObject == OBJID_WINDOW) {
        if (GlobalState::g_TargetHwnd != NULL) {
            if (hwnd == GlobalState::g_TargetHwnd) {
                LogMessage("INFO", "WinEventHook 捕获到目标窗口销毁事件 (HWND: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + ")");
                GlobalState::g_AppClosed = true;
                PostQuitMessage(0);
            }
        } else {
            wchar_t className[256];
            ZeroMemory(className, sizeof(className));
            GetClassNameW(hwnd, className, 256);
            if (wcscmp(className, Config::APP_WINDOW_CLASS) == 0) {
                LogMessage("INFO", "WinEventHook 捕获到 Edge/Chrome 顶层窗口销毁事件");
                GlobalState::g_AppClosed = true;
                PostQuitMessage(0);
            }
        }
    }
}

HWINEVENTHOOK InstallWindowDestroyHook(HWND targetHwnd, DWORD targetPid) {
    GlobalState::g_TargetHwnd = targetHwnd;
    GlobalState::g_TargetPid = targetPid;

    HWINEVENTHOOK hHook = SetWinEventHook(
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_DESTROY,
        NULL,
        WinEventProcCallback,
        targetPid,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (hHook == NULL) {
        LogMessage("WARN", "SetWinEventHook 目标 PID 注册失败，尝试全局注册");
        hHook = SetWinEventHook(
            EVENT_OBJECT_DESTROY,
            EVENT_OBJECT_DESTROY,
            NULL,
            WinEventProcCallback,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );
    } else {
        LogMessage("INFO", "WinEventHook 注册成功 (EVENT_OBJECT_DESTROY)");
    }
    return hHook;
}

static DWORD WINAPI ConsoleDrainThreadProc(LPVOID param) {
    HANDLE hRead = static_cast<HANDLE>(param);
    char buf[4096];
    for (;;) {
        DWORD read = 0;
        BOOL ok = ReadFile(hRead, buf, sizeof(buf), &read, NULL);
        if (!ok || read == 0) break;
        WriteConsoleLog(buf, read);
    }
    CloseHandle(hRead);
    return 0;
}

void StartConsoleDrainThread(HANDLE hStdoutRead) {
    if (hStdoutRead == NULL) return;
    HANDLE thread = CreateThread(NULL, 0, ConsoleDrainThreadProc, hStdoutRead, 0, NULL);
    if (thread == NULL) {
        LogMessage("WARN", "无法启动 console 日志线程，LastError: " + std::to_string(GetLastError()));
        CloseHandle(hStdoutRead);
        return;
    }
    GlobalState::g_DrainThread = thread;
}

void RunEventDrivenLifecyclePump(HWND targetHwnd, DWORD targetPid, HANDLE hBackendProcess) {
    HWINEVENTHOOK hHook = InstallWindowDestroyHook(targetHwnd, targetPid);

    LogMessage("INFO", "进入 MsgWaitForMultipleObjects 内核挂起等待 (0 轮询 / 0% CPU) ...");

    if (targetHwnd != NULL && !IsWindow(targetHwnd)) {
        LogMessage("INFO", "挂钩时检测到目标窗口已无效，立即退出");
        if (hHook) UnhookWinEvent(hHook);
        return;
    }

    bool running = true;
    while (running && !GlobalState::g_AppClosed) {
        DWORD waitRes = MsgWaitForMultipleObjects(
            1,
            &hBackendProcess,
            FALSE,
            INFINITE,
            QS_ALLINPUT
        );

        if (waitRes == WAIT_OBJECT_0) {
            LogMessage("WARN", "后台服务进程句柄被置位，服务已终止");
            running = false;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    LogMessage("INFO", "接收到 WM_QUIT 退出消息");
                    running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (hHook) {
        UnhookWinEvent(hHook);
        LogMessage("INFO", "已注销 WinEventHook");
    }
}

void CleanupBackendProcess(HANDLE hJob, HANDLE hProcess, HANDLE hStdoutRead) {
    LogMessage("INFO", "执行后台进程树彻底清理 ...");
    if (hJob != NULL) {
        char buffer[4096];
        ZeroMemory(buffer, sizeof(buffer));
        if (QueryInformationJobObject(hJob, JobObjectBasicProcessIdList, buffer, sizeof(buffer), NULL)) {
            auto* pList = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer);
            LogMessage("INFO", "Job Object 内共发现 " + std::to_string(pList->NumberOfProcessIdsInList) + " 个关联进程，正在执行终结");
        }
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
    }
    if (hProcess != NULL) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
    }
    if (hStdoutRead != NULL) {
        CloseHandle(hStdoutRead);
    }
    if (GlobalState::g_DrainThread != NULL) {
        WaitForSingleObject(GlobalState::g_DrainThread, 1000);
        CloseHandle(GlobalState::g_DrainThread);
        GlobalState::g_DrainThread = NULL;
    }
    if (GlobalState::g_ConsoleLog != NULL) {
        CloseHandle(GlobalState::g_ConsoleLog);
        GlobalState::g_ConsoleLog = NULL;
    }
}
