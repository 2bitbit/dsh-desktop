/**
 * @file dsh_pwa_shim.cpp
 * @brief DSH PWA 静默守护与生命周期联动 Shim (快捷方式参数驱动 + 直通 Node 极速版)
 * 
 * 功能职责：
 * 1. 后台工作目录由快捷方式参数 (--cwd <dir>) 提供，0 硬编码。
 * 2. 直通 Node.js 极速启动：跳过 PowerShell 及 Profile 加载。
 * 3. 显式绑定标准 NUL 句柄以防止 Node.js 在无窗口模式下发生 stdout 管道异常崩溃。
 * 4. 将后台进程严格绑定入 Windows Job Object (禁止 BREAKAWAY，配置 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)。
 * 5. 30ms 高频非阻塞探测 127.0.0.1:3080 服务端口，30ms 极速捕获 PWA 窗口。
 * 6. 唤起 PWA 窗口并捕获其 HWND 句柄与宿主 PID。
 * 7. 基于 Win32 原生 SetWinEventHook + MsgWaitForMultipleObjects 纯事件驱动挂起等待 (0 轮询 / 0% CPU)。
 * 8. PWA 窗口关闭瞬间：通过 TerminateJobObject 强制秒级全量清理 Job Object 树下的所有 node 进程，释放 3080 端口。
 * 9. 日志路径动态绑定至 exe 所在同级目录下的 dsh_pwa_shim.log，内置 512KB 磁盘防爆。
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#endif

// ==========================================
// 常量与配置定义 (集中优于分散，SSOT 原则)
// ==========================================
namespace Config {
    // PWA 启动路径与参数
    constexpr const wchar_t* PWA_EXE_PATH    = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge_proxy.exe";
    constexpr const wchar_t* PWA_EXE_ARGS    = L"\"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge_proxy.exe\" --profile-directory=Default --app-id=hgiemfgfjhalibdoboikeiepnnjapnpc --app-url=http://127.0.0.1:3080/ --app-launch-source=4";
    
    // 窗口匹配特征
    constexpr const wchar_t* PWA_WINDOW_CLASS = L"Chrome_WidgetWin_1";
    constexpr const wchar_t* PWA_TITLE_KW1   = L"DeepSeek";
    constexpr const wchar_t* PWA_TITLE_KW2   = L"Harness";
    constexpr const wchar_t* PWA_TITLE_KW3   = L"dsh";
    constexpr const wchar_t* PWA_TITLE_KW4   = L"3080";
    
    // 日志文件名与防爆阈值 (512 KB)
    constexpr const wchar_t* LOG_FILE_NAME   = L"dsh_pwa_shim.log";
    constexpr DWORD MAX_LOG_SIZE_BYTES       = 512 * 1024;

    // 网络就绪检测参数 (30ms 极速探测)
    constexpr const char* HEALTH_HOST        = "127.0.0.1";
    constexpr unsigned short HEALTH_PORT     = 3080;
    constexpr DWORD HEALTH_TIMEOUT_MS        = 15000;
    constexpr DWORD HEALTH_INTERVAL_MS       = 30;

    // PWA 窗口锁定超时与 30ms 极速扫描
    constexpr DWORD PWA_CAPTURE_TIMEOUT_MS   = 15000;
    constexpr DWORD PWA_CAPTURE_INTERVAL_MS  = 30;
}

// ==========================================
// 全局状态 (事件钩子回调与主线程通信)
// ==========================================
namespace GlobalState {
    static HWND g_TargetHwnd = NULL;
    static DWORD g_TargetPid = 0;
    static bool g_PwaClosed = false;
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
 * @brief 初始化 Windows Job Object (严禁 Breakaway，强制 KillOnClose)。
 * @return 创建成功的 Job 句柄，若失败返回 NULL。
 */
HANDLE InitializeJobObject();

/**
 * @brief 动态解析 npm / pnpm / yarn 全局目录下的 dsh bin.js 与 NODE_PATH，实现跳过 Shell 直连 Node.js。
 * @param[out] outNodeExe node.exe 路径。
 * @param[out] outBinJs dsh bin.js 路径。
 * @param[out] outNodePath NODE_PATH 依赖目录。
 * @return 解析是否成功。
 */
bool ResolveDshDirectPaths(std::wstring& outNodeExe, std::wstring& outBinJs, std::wstring& outNodePath);

/**
 * @brief 启动后台 dsh web 服务 (直通 Node.js 或安全级联回退至 pwsh / powershell / cmd)。
 * @param workingDir 目标工作目录。
 * @param[out] pi 存储创建出的进程与主线程句柄。
 * @return 启动是否成功。
 */
bool LaunchBackendProcess(const std::wstring& workingDir, PROCESS_INFORMATION& pi);

/**
 * @brief 轮询检测 TCP 端口是否处于监听并可连接状态 (30ms 极速探测)。
 * @param host 探测主机 IP (如 "127.0.0.1")。
 * @param port 探测端口号 (如 3080)。
 * @param timeoutMs 超时时间 (毫秒)。
 * @param hProcess 后台进程句柄 (用于 fail-fast 检查其是否提前异常退出)。
 * @return 服务是否已准备就绪。
 */
bool WaitForPortReady(const char* host, unsigned short port, DWORD timeoutMs, HANDLE hProcess);

/**
 * @brief 收集当前系统中所有现存匹配特征的窗口句柄。
 * @return 窗口 HWND 列表。
 */
std::vector<HWND> SnapshotExistingPwaWindows();

/**
 * @brief 唤起 Edge PWA 客户端。
 * @return 唤起是否成功。
 */
bool LaunchPwaClient();

/**
 * @brief 捕获新弹出的 PWA 窗口句柄与其宿主 PID (30ms 高频扫描)。
 * @param oldHwnds 启动前已存在的窗口句柄列表。
 * @param timeoutMs 查找超时时间 (毫秒)。
 * @param[out] outPid 输出目标窗口所属进程 PID。
 * @return 成功捕获到的 PWA 窗口 HWND，超时则返回 NULL。
 */
HWND CapturePwaWindowWithPid(const std::vector<HWND>& oldHwnds, DWORD timeoutMs, DWORD& outPid);

/**
 * @brief 纯事件驱动的主监控事件泵。
 * @param targetHwnd 目标 PWA 窗口句柄。
 * @param targetPid 目标 PWA 进程 PID。
 * @param hBackendProcess 后台服务进程句柄。
 */
void RunEventDrivenLifecyclePump(HWND targetHwnd, DWORD targetPid, HANDLE hBackendProcess);

/**
 * @brief 彻底终止并清理 Job Object 及后台进程树。
 * @param hJob 作业对象句柄。
 * @param hProcess 后台主进程句柄。
 */
void CleanupBackendProcess(HANDLE hJob, HANDLE hProcess);

/**
 * @brief 检查命令行中是否含有 --debug 参数。
 * @param lpCmdLine 命令行字符串。
 * @return 是否开启 debug 模式。
 */
bool IsDebugMode(LPCSTR lpCmdLine);

// ==========================================
// 报纸结构：主流程入口 (WinMain)
// ==========================================

/**
 * @brief Win32 GUI 入口点，彻底免去控制台黑框生成。
 */
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)nCmdShow;

    // 0. 初始化 COM 环境
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    bool debug = IsDebugMode(lpCmdLine);
    LogMessage("INFO", "========== DSH PWA Shim 启动 ==========");
    if (debug) {
        LogMessage("DEBUG", "已启用调试模式");
    }

    // 1. 动态解析快捷方式参数传递的工作目录 (--cwd <path>)
    std::wstring workingDir = ResolveTargetWorkingDirectory();
    LogMessage("INFO", "目标后台工作目录 (来自快捷方式参数): " + ToUtf8(workingDir));

    // 2. 初始化 Winsock 环境
    WSADATA wsaData;
    int wsaRes = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRes != 0) {
        LogMessage("ERROR", "WSAStartup 初始化失败，错误码: " + std::to_string(wsaRes));
        if (debug) MessageBoxW(NULL, L"WSAStartup 初始化失败", L"DSH Shim 错误", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // 3. 创建并配置严格隔离的 Job Object
    HANDLE hJob = InitializeJobObject();
    if (!hJob) {
        LogMessage("ERROR", "Job Object 创建或配置失败，LastError: " + std::to_string(GetLastError()));
        if (debug) MessageBoxW(NULL, L"Job Object 创建失败", L"DSH Shim 错误", MB_ICONERROR);
        WSACleanup();
        CoUninitialize();
        return 2;
    }
    LogMessage("INFO", "严格隔离 Job Object 配置完成");

    // 4. 静默拉起后台 dsh web 服务 (直通 Node 极速启动)
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!LaunchBackendProcess(workingDir, pi)) {
        LogMessage("ERROR", "LaunchBackendProcess 启动失败，LastError: " + std::to_string(GetLastError()));
        if (debug) MessageBoxW(NULL, L"后台 dsh web 服务启动失败，请检查 Node/dsh 安装", L"DSH Shim 错误", MB_ICONERROR);
        CloseHandle(hJob);
        WSACleanup();
        CoUninitialize();
        return 3;
    }
    LogMessage("INFO", "后台服务进程已创建 (PID: " + std::to_string(pi.dwProcessId) + ")");

    // 5. 将后台服务进程绑定入 Job Object，随后恢复主线程运行
    if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
        LogMessage("ERROR", "AssignProcessToJobObject 绑定失败，LastError: " + std::to_string(GetLastError()));
    } else {
        LogMessage("INFO", "已成功将后台服务进程绑定至 Job Object 树");
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // 6. 30ms 极速健康检查：等待 127.0.0.1:3080 端口启动就绪
    LogMessage("INFO", "正在等待后台服务监听 127.0.0.1:3080 (30ms 判定) ...");
    bool isReady = WaitForPortReady(Config::HEALTH_HOST, Config::HEALTH_PORT, Config::HEALTH_TIMEOUT_MS, pi.hProcess);
    if (!isReady) {
        LogMessage("ERROR", "后台服务在指定超时时间内未能就绪或已提前崩溃");
        if (debug) MessageBoxW(NULL, L"后台服务在超时时间内未能就绪或已崩溃，请查看日志", L"DSH Shim 错误", MB_ICONERROR);
        CleanupBackendProcess(hJob, pi.hProcess);
        WSACleanup();
        CoUninitialize();
        return 4;
    }
    LogMessage("INFO", "后台服务 127.0.0.1:3080 端口已就绪");

    // 7. 记录启动前的窗口快照，并唤起 Edge PWA
    std::vector<HWND> oldHwnds = SnapshotExistingPwaWindows();
    LogMessage("INFO", "启动前匹配窗口快照数量: " + std::to_string(oldHwnds.size()));

    if (!LaunchPwaClient()) {
        LogMessage("ERROR", "唤起 PWA 客户端失败");
        if (debug) MessageBoxW(NULL, L"唤起 PWA 客户端失败", L"DSH Shim 错误", MB_ICONERROR);
        CleanupBackendProcess(hJob, pi.hProcess);
        WSACleanup();
        CoUninitialize();
        return 5;
    }
    LogMessage("INFO", "已触发 Edge PWA 唤起请求");

    // 8. 捕获 PWA 窗口句柄及其所属进程 PID (30ms 高频捕获)
    DWORD targetPid = 0;
    HWND targetHwnd = CapturePwaWindowWithPid(oldHwnds, Config::PWA_CAPTURE_TIMEOUT_MS, targetPid);
    if (targetHwnd != NULL) {
        LogMessage("INFO", "已锁定 PWA 目标窗口 HWND: " + std::to_string(reinterpret_cast<uintptr_t>(targetHwnd)) + ", PID: " + std::to_string(targetPid));
    } else {
        LogMessage("WARN", "未能精确锁定特定 HWND，启用全局窗口销毁事件监听");
    }

    // 9. 启动纯事件驱动生命周期泵 (挂起等待，0 轮询 / 0% CPU)
    RunEventDrivenLifecyclePump(targetHwnd, targetPid, pi.hProcess);

    // 10. PWA 已关闭，执行后台进程树全量强制清理
    LogMessage("INFO", "PWA 窗口已销毁，正在强制清理后台进程树 ...");
    CleanupBackendProcess(hJob, pi.hProcess);
    WSACleanup();
    CoUninitialize();
    LogMessage("INFO", "========== DSH PWA Shim 正常退出 (后台已完全回收) ==========");

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

bool IsDebugMode(LPCSTR lpCmdLine) {
    if (lpCmdLine == NULL) return false;
    std::string cmd(lpCmdLine);
    return (cmd.find("--debug") != std::string::npos || cmd.find("-d") != std::string::npos);
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

static std::wstring ResolveNodeExecutable() {
    wchar_t buf[MAX_PATH];

    // 1. pnpm 管理的 node.exe
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        std::wstring pnpmNode = std::wstring(buf) + L"\\pnpm\\bin\\node.exe";
        if (PathFileExistsW(pnpmNode.c_str())) return pnpmNode;
    }
    if (GetEnvironmentVariableW(L"PNPM_HOME", buf, MAX_PATH) > 0) {
        std::wstring pnpmNode = std::wstring(buf) + L"\\node.exe";
        if (PathFileExistsW(pnpmNode.c_str())) return pnpmNode;
    }

    // 2. 官方标准安装路径 (64位 / 32位)
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH) > 0) {
        std::wstring standardNode = std::wstring(buf) + L"\\nodejs\\node.exe";
        if (PathFileExistsW(standardNode.c_str())) return standardNode;
    }
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH) > 0) {
        std::wstring standardNode = std::wstring(buf) + L"\\nodejs\\node.exe";
        if (PathFileExistsW(standardNode.c_str())) return standardNode;
    }

    // 3. 依赖系统 PATH
    return L"node.exe";
}

bool ResolveDshDirectPaths(std::wstring& outNodeExe, std::wstring& outBinJs, std::wstring& outNodePath) {
    outNodeExe = ResolveNodeExecutable();
    wchar_t buf[MAX_PATH];

    // 1. 优先探测 npm 全局安装路径 (%APPDATA%\npm\node_modules\@deepseek-ai\dsh\lib\bin.js)
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0) {
        std::wstring npmModules = std::wstring(buf) + L"\\npm\\node_modules";
        std::wstring npmBin = npmModules + L"\\@deepseek-ai\\dsh\\lib\\bin.js";
        if (PathFileExistsW(npmBin.c_str())) {
            outBinJs = npmBin;
            outNodePath = npmModules;
            return true;
        }
    }

    // 2. 探测 pnpm 全局安装路径 (%LOCALAPPDATA%\pnpm\global\*)
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        std::wstring baseGlobal = std::wstring(buf) + L"\\pnpm\\global\\";
        std::wstring searchPattern = baseGlobal + L"*";

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                    continue;
                }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // 检查直接位于版本目录下，或版本目录的下级哈希目录
                    std::wstring versionDir = baseGlobal + fd.cFileName;
                    std::wstring candBin = versionDir + L"\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js";
                    std::wstring candNodePath = versionDir + L"\\node_modules\\.pnpm\\node_modules";

                    if (PathFileExistsW(candBin.c_str())) {
                        outBinJs = candBin;
                        outNodePath = candNodePath;
                        FindClose(hFind);
                        return true;
                    }

                    // 探测 global/<ver>/<hash>/... 结构
                    std::wstring subPattern = versionDir + L"\\*";
                    WIN32_FIND_DATAW subFd;
                    HANDLE hSubFind = FindFirstFileW(subPattern.c_str(), &subFd);
                    if (hSubFind != INVALID_HANDLE_VALUE) {
                        do {
                            if (subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                                if (wcscmp(subFd.cFileName, L".") == 0 || wcscmp(subFd.cFileName, L"..") == 0) continue;
                                std::wstring hashDir = versionDir + L"\\" + subFd.cFileName;
                                std::wstring subCandBin = hashDir + L"\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js";
                                std::wstring subCandNodePath = hashDir + L"\\node_modules\\.pnpm\\node_modules";
                                if (PathFileExistsW(subCandBin.c_str())) {
                                    outBinJs = subCandBin;
                                    outNodePath = subCandNodePath;
                                    FindClose(hSubFind);
                                    FindClose(hFind);
                                    return true;
                                }
                            }
                        } while (FindNextFileW(hSubFind, &subFd));
                        FindClose(hSubFind);
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    // 3. 探测 yarn 全局安装路径 (%LOCALAPPDATA%\Yarn\Data\global\node_modules\@deepseek-ai\dsh\lib\bin.js)
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        std::wstring yarnModules = std::wstring(buf) + L"\\Yarn\\Data\\global\\node_modules";
        std::wstring yarnBin = yarnModules + L"\\@deepseek-ai\\dsh\\lib\\bin.js";
        if (PathFileExistsW(yarnBin.c_str())) {
            outBinJs = yarnBin;
            outNodePath = yarnModules;
            return true;
        }
    }

    return false;
}

bool LaunchBackendProcess(const std::wstring& workingDir, PROCESS_INFORMATION& pi) {
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

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdOutput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdError = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;

    DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

    // 阶段 1：直通 Node.js 极速启动 (npm / pnpm / yarn 全局安装探测)
    std::wstring nodeExe, binJs, nodePath;
    if (ResolveDshDirectPaths(nodeExe, binJs, nodePath)) {
        LogMessage("INFO", "成功动态定位到 DSH 运行文件 (启用 Direct Node.js 极速直通启动)");
        
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
            creationFlags,
            NULL,
            workingDir.c_str(),
            &si,
            &pi
        );

        if (success) {
            if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
            return true;
        }
        LogMessage("WARN", "Direct Node.js 启动失败，正在回退至系统 Shell 启动");
    }

    // 阶段 2：多级 Shell 级联回退 (pwsh -> powershell -> cmd)
    const std::vector<std::pair<std::string, std::wstring>> fallbackCommands = {
        {"pwsh (PowerShell 7)", L"pwsh.exe -ExecutionPolicy Bypass -Command \"dsh web --no-open\""},
        {"powershell (Windows PowerShell)", L"powershell.exe -ExecutionPolicy Bypass -Command \"dsh web --no-open\""},
        {"cmd (Windows Command Prompt)", L"cmd.exe /d /c \"dsh web --no-open\""}
    };

    for (const auto& entry : fallbackCommands) {
        std::wstring cmdLine = entry.second;
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        BOOL success = CreateProcessW(
            NULL,
            cmdBuf.data(),
            NULL,
            NULL,
            TRUE,
            creationFlags,
            NULL,
            workingDir.c_str(),
            &si,
            &pi
        );

        if (success) {
            LogMessage("INFO", "通过回退解释器 " + entry.first + " 成功创建后台进程");
            if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
            return true;
        }
    }

    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    return false;
}

bool WaitForPortReady(const char* host, unsigned short port, DWORD timeoutMs, HANDLE hProcess) {
    DWORD startTick = GetTickCount();

    while ((GetTickCount() - startTick) < timeoutMs) {
        if (hProcess != NULL) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                LogMessage("ERROR", "后台服务进程已提前退出，ExitCode: " + std::to_string(exitCode));
                return false;
            }
        }

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            sockaddr_in addr;
            ZeroMemory(&addr, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, host, &addr.sin_addr);

            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);

            int res = connect(s, (sockaddr*)&addr, sizeof(addr));
            if (res == 0) {
                closesocket(s);
                return true;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = Config::HEALTH_INTERVAL_MS * 1000;

            if (select(0, NULL, &writeSet, NULL, &tv) > 0) {
                int sockErr = 0;
                int optLen = sizeof(sockErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&sockErr, &optLen);
                closesocket(s);
                if (sockErr == 0) {
                    return true;
                }
            } else {
                closesocket(s);
            }
        }

        Sleep(Config::HEALTH_INTERVAL_MS);
    }
    return false;
}

struct WindowSearchContext {
    std::vector<HWND> results;
};

static bool MatchesPwaFeatures(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) {
        return false;
    }

    wchar_t className[256];
    ZeroMemory(className, sizeof(className));
    GetClassNameW(hwnd, className, 256);

    if (wcscmp(className, Config::PWA_WINDOW_CLASS) == 0) {
        return true;
    }
    return false;
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    if (MatchesPwaFeatures(hwnd)) {
        WindowSearchContext* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
        ctx->results.push_back(hwnd);
    }
    return TRUE;
}

std::vector<HWND> SnapshotExistingPwaWindows() {
    WindowSearchContext ctx;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));
    return ctx.results;
}

bool LaunchPwaClient() {
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::wstring cmdLine = Config::PWA_EXE_ARGS;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL success = CreateProcessW(
        Config::PWA_EXE_PATH,
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

    if (success) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    LogMessage("WARN", "CreateProcessW 启动 msedge_proxy 失败，尝试 ShellExecuteW");

    HINSTANCE hInst = ShellExecuteW(
        NULL,
        L"open",
        Config::PWA_EXE_PATH,
        L"--profile-directory=Default --app-id=hgiemfgfjhalibdoboikeiepnnjapnpc --app-url=http://127.0.0.1:3080/ --app-launch-source=4",
        NULL,
        SW_SHOWNORMAL
    );
    return (reinterpret_cast<INT_PTR>(hInst) > 32);
}

HWND CapturePwaWindowWithPid(const std::vector<HWND>& oldHwnds, DWORD timeoutMs, DWORD& outPid) {
    DWORD startTick = GetTickCount();

    while ((GetTickCount() - startTick) < timeoutMs) {
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
        Sleep(Config::PWA_CAPTURE_INTERVAL_MS);
    }

    for (HWND hwnd : SnapshotExistingPwaWindows()) {
        wchar_t title[512];
        ZeroMemory(title, sizeof(title));
        GetWindowTextW(hwnd, title, 512);
        std::wstring titleStr(title);
        if (titleStr.find(Config::PWA_TITLE_KW1) != std::wstring::npos ||
            titleStr.find(Config::PWA_TITLE_KW2) != std::wstring::npos ||
            titleStr.find(Config::PWA_TITLE_KW3) != std::wstring::npos ||
            titleStr.find(Config::PWA_TITLE_KW4) != std::wstring::npos) {
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
                LogMessage("INFO", "WinEventHook 捕获到目标 PWA 窗口销毁事件 (HWND: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + ")");
                GlobalState::g_PwaClosed = true;
                PostQuitMessage(0);
            }
        } else {
            wchar_t className[256];
            ZeroMemory(className, sizeof(className));
            GetClassNameW(hwnd, className, 256);
            if (wcscmp(className, Config::PWA_WINDOW_CLASS) == 0) {
                LogMessage("INFO", "WinEventHook 捕获到 Edge/Chrome 顶层窗口销毁事件");
                GlobalState::g_PwaClosed = true;
                PostQuitMessage(0);
            }
        }
    }
}

void RunEventDrivenLifecyclePump(HWND targetHwnd, DWORD targetPid, HANDLE hBackendProcess) {
    GlobalState::g_TargetHwnd = targetHwnd;
    GlobalState::g_TargetPid = targetPid;
    GlobalState::g_PwaClosed = false;

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

    LogMessage("INFO", "进入 MsgWaitForMultipleObjects 内核挂起等待 (0 轮询 / 0% CPU) ...");

    if (targetHwnd != NULL && !IsWindow(targetHwnd)) {
        LogMessage("INFO", "挂钩时检测到目标窗口已无效，立即退出");
        if (hHook) UnhookWinEvent(hHook);
        return;
    }

    bool running = true;
    while (running && !GlobalState::g_PwaClosed) {
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

void CleanupBackendProcess(HANDLE hJob, HANDLE hProcess) {
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
}
