/**
 * @file dsh_pwa_shim.cpp
 * @brief DSH Web 静默守护与生命周期联动 Shim（快捷方式参数驱动 + 直通 Node + 托盘常驻）
 *
 * 功能职责：
 * 1. 后台工作目录由快捷方式参数 (--cwd <dir>) 提供，0 硬编码。
 * 2. 直通 Node.js 启动：从 pnpm 的 dsh shim 读取 cmd-shim-target，跳过 PowerShell。
 * 3. 将 Node stdout/stderr 接到匿名管道（同时避免无窗口模式下的 stdout 崩溃）。
 * 4. 将后台进程绑定入 Windows Job Object（JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE）。
 * 5. 单实例：隐藏控制窗 + FindWindow 判定；重复运行只前置已有窗口，不再起第二套后台。
 * 6. 立刻显示原生启动窗（不打开无 token 的页面，避免 404/401）。
 * 7. 从 stdout 解析 `dsh web: http://127.0.0.1:<port>/?token=...`（0.1.2 起根路径强制认证）。
 * 8. token 就绪后只拉起一次 Edge --app=<tokenUrl>，用户第一眼是 DSH 自己的加载页。
 * 9. 捕获窗口 HWND/PID 并立即最大化；SetWinEventHook + MsgWaitForMultipleObjects 事件驱动等待。
 * 10. 关闭语义（WH_MOUSE_LL / WH_KEYBOARD_LL 拦截，详见 IsCloseIntercepted / IsHideHotkey）：
 *     - 左键点标题栏叉号、Alt+F4、Ctrl+Shift+W → 窗口隐藏到托盘，后台继续运行（都不弹任何通知）；
 *     - 右键托盘菜单「退出」→ 关闭应用窗口并回收后台进程树；
 *     - 窗口被其它途径销毁（任务栏预览关闭、浏览器崩溃等）→ 同样回收后台再退出，不留僵尸进程。
 * 11. 托盘图标：左键单击前置窗口；右键弹出菜单；图标内嵌自 IDI_DSH_TRAY 资源；Explorer 重启后自动重建。
 * 12. 进程按「每显示器 DPI 感知 v2」运行：低层钩子给物理坐标，DPI 不感知会让关闭按钮判定差一个缩放比。
 * 13. 每次启动清空 exe 同级 dsh_pwa_shim.log / dsh_web_console.log；运行中追加，分别 512KB / 2MB 防爆。token 入日志时脱敏。
 *
 * 构建：
 *   1) 托盘图标是内嵌资源：图标 src/tray_icon.ico，资源脚本 src/dsh_pwa_shim.rc
 *      （其 ID 必须与 Config::IDI_DSH_TRAY 一致；windres 以 .rc 所在目录解析图标路径）。
 *   2) 直接跑 scripts/build.ps1：自动编译资源 + 自动挑 GCC / Clang / MSVC。
 *   3) 手工等价命令（MinGW-w64 GCC，静态链接，成品不依赖 MinGW 运行时 DLL）：
 *        windres -c 65001 src/dsh_pwa_shim.rc -O coff -o bin/dsh_pwa_shim.res
 *        g++ -O2 -std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ \
 *            src/dsh_pwa_shim.cpp bin/dsh_pwa_shim.res -o bin/dsh_pwa_shim.exe \
 *            -lshlwapi -lshell32 -lole32 -luser32 -lgdi32
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
#include <cstring>
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
    constexpr int SPLASH_WIDTH_LOGICAL       = 440;
    constexpr int SPLASH_HEIGHT_LOGICAL      = 240;
    constexpr int SPLASH_FONT_HEIGHT_LOGICAL = 12;
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

    // ---- 单实例 / 托盘 / 关闭拦截 ----
    constexpr const wchar_t* CONTROLLER_CLASS = L"DshPwaShimController";
    constexpr const wchar_t* CONTROLLER_TITLE = L"DSH Desktop Shim Controller";
    constexpr const wchar_t* TASKBAR_CREATED_MSG = L"TaskbarCreated";
    constexpr const wchar_t* TRAY_TIP         = L"DeepSeek Harness（左键显示 / 右键菜单 / Ctrl+Shift+W 收起）";
    constexpr UINT WM_DSH_ACTIVATE            = WM_APP + 1;   // 第二个实例请求前置窗口
    constexpr UINT WM_DSH_TRAY                = WM_APP + 2;   // 托盘图标回调
    constexpr UINT WM_DSH_HIDDEN              = WM_APP + 3;   // 刚刚隐藏到托盘（只记日志，不弹任何通知）
    constexpr UINT TRAY_ICON_ID               = 1;
    constexpr UINT TRAY_MENU_SHOW             = 1;
    constexpr UINT TRAY_MENU_QUIT             = 2;
    constexpr int  IDI_DSH_TRAY               = 101;          // 必须与 dsh_pwa_shim.rc 中的 ID 一致
    constexpr DWORD HIDE_HOTKEY_VK            = 'W';          // Ctrl+Shift+W：等价于左键点叉号
    constexpr int  CAPTION_BAND_LOGICAL_PX    = 120;          // 只在窗口顶部这条带内做关闭按钮判定
    constexpr DWORD HIT_TEST_TIMEOUT_MS       = 100;
}

// ==========================================
// 动态解析的 user32 导出（DPI 相关，Windows 10 1607 起必备）
// ==========================================
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);

// ==========================================
// 全局状态 (事件钩子回调与主线程通信)
// ==========================================
namespace GlobalState {
    static HINSTANCE g_Instance = NULL;
    static HWND g_TargetHwnd = NULL;
    static DWORD g_TargetPid = 0;
    static bool g_AppClosed = false;
    static HANDLE g_ConsoleLog = NULL;
    static HANDLE g_DrainThread = NULL;
    static HWND g_SplashHwnd = NULL;
    static bool g_SplashCancelled = false;
    static HFONT g_SplashFont = NULL;
    static wchar_t g_SplashStatus[160];

    static HWND g_ControllerHwnd = NULL;
    static UINT g_TaskbarCreatedMsg = 0;
    static HHOOK g_MouseHook = NULL;
    static HHOOK g_KeyboardHook = NULL;
    static bool g_SwallowButtonUp = false;
    static bool g_CloseInterceptReady = false;
    static bool g_TrayAdded = false;
    static HICON g_TrayIcon = NULL;
    static UINT g_SystemDpi = 96;
    static PFN_GetDpiForWindow g_GetDpiForWindow = NULL;
}

// ==========================================
// 符号声明 (报纸结构前置)
// ==========================================

/**
 * @brief 开启每显示器 DPI 感知 v2 并缓存系统 DPI 与窗口 DPI 查询入口。
 * @return 必需 API 是否可用；不可用时调用方应 fail-fast。
 */
bool SetupDpiAwareness();

/**
 * @brief 把逻辑像素换算成当前系统 DPI 下的物理像素（启动窗尺寸与字体用）。
 * @param logical 逻辑像素。
 * @return 物理像素。
 */
int DpiScale(int logical);

/**
 * @brief 从 user32.dll 动态取函数指针（用 memcpy 规避 -Wcast-function-type 告警）。
 * @tparam T 目标函数指针类型。
 * @param name 导出名。
 * @return 函数指针，缺失时为 NULL。
 */
template <typename T>
T LoadUser32(const char* name);

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
 * @brief 单实例判定：若已有实例在跑，把前台特权转交给它并请它前置窗口。
 * @return true 表示已有实例接管，本进程应立即退出。
 */
bool ActivateExistingInstance();

/**
 * @brief 创建隐藏控制窗：单实例锚点 + 托盘回调 + 跨实例消息宿主。
 * @param hInstance 当前模块实例。
 * @return 控制窗 HWND，失败返回 NULL。
 */
HWND CreateControllerWindow(HINSTANCE hInstance);

/**
 * @brief 控制窗过程：处理托盘回调、第二个实例的前置请求、托盘重建。
 */
LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/**
 * @brief 安装关闭语义拦截钩子（低层鼠标 + 低层键盘）。
 * @return 两个钩子是否都安装成功。
 */
bool InstallCloseInterceptors();

/**
 * @brief 卸载关闭语义拦截钩子。
 */
void UninstallCloseInterceptors();

/**
 * @brief 低层鼠标钩子：命中标题栏叉号则隐藏到托盘并吞掉这次点击。
 */
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

/**
 * @brief 低层键盘钩子：应用窗口在前台时把「隐藏热键」转成隐藏到托盘。
 */
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

/**
 * @brief 判断这次按键是不是隐藏热键（Alt+F4 或 Ctrl+Shift+W）。
 * @param kb 低层键盘钩子给出的按键信息。
 * @return 是否命中热键。
 */
bool IsHideHotkey(const KBDLLHOOKSTRUCT* kb);

/**
 * @brief 判断应用窗口此刻是不是前台窗口（热键只在应用窗口受控时才吞，不抢别的程序的快捷键）。
 * @return 应用窗口是否处于前台。
 */
bool IsAppWindowForeground();

/**
 * @brief 判断某个屏幕点位是否是一次「点叉号」。
 * @param pt 屏幕物理坐标（低层钩子给出的原始坐标）。
 * @return 是否应拦截。
 */
bool IsCloseIntercepted(const POINT& pt);

/**
 * @brief 把应用窗口隐藏到托盘（钩子回调里只做最小动作）。
 */
void HideMainWindowToTray();

/**
 * @brief 前置应用窗口；窗口还没建好时前置启动窗。
 */
void ActivateMainWindow();

/**
 * @brief 处理托盘图标的鼠标回调。
 * @param lParam 回调消息的 lParam（低 16 位为鼠标消息）。
 */
void HandleTrayCallback(LPARAM lParam);

/**
 * @brief 在光标处弹出托盘右键菜单。
 */
void ShowTrayMenu();

/**
 * @brief 托盘菜单「退出」：关闭应用窗口并让主循环走回收路径。
 */
void RequestQuit();

/**
 * @brief 添加托盘图标（图标取自内嵌资源 IDI_DSH_TRAY）。
 * @return 是否添加成功。
 */
bool AddTrayIcon();

/**
 * @brief 移除托盘图标并释放图标句柄。
 */
void RemoveTrayIcon();

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
 * @brief 销毁原生启动窗并释放其字体。
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
    (void)hPrevInstance;
    (void)nCmdShow;

    GlobalState::g_Instance = hInstance;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    bool debug = IsDebugMode(lpCmdLine);

    // 前提检查 1：DPI 感知 API。缺了它，低层钩子的物理坐标与窗口矩形会差一个缩放比。
    if (!SetupDpiAwareness()) {
        MessageBoxW(NULL, L"本程序需要 Windows 10 1607 及以上（缺少 DPI 感知 API），无法保证关闭按钮判定准确。",
                    L"DSH Shim 环境检查失败", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // 前提检查 2：单实例。必须早于 TruncateShimLog，否则会把正在运行实例的日志清空。
    if (ActivateExistingInstance()) {
        CoUninitialize();
        return 0;
    }

    TruncateShimLog();
    GlobalState::g_ConsoleLog = OpenConsoleLog();
    if (GlobalState::g_ConsoleLog == NULL) {
        LogMessage("WARN", "无法创建 dsh_web_console.log，dsh web 输出不会落盘");
    }
    LogMessage("INFO", "========== DSH Desktop Shim 启动 ==========");
    LogMessage("INFO", "进程已开启每显示器 DPI 感知 v2，系统 DPI: " + std::to_string(GlobalState::g_SystemDpi));
    if (debug) {
        LogMessage("DEBUG", "已启用调试模式");
    }

    // 前提检查 3：控制窗 + 关闭语义钩子。窗口还没起来，失败也不影响后台。
    if (CreateControllerWindow(hInstance) == NULL) {
        LogMessage("ERROR", "创建控制窗失败，LastError: " + std::to_string(GetLastError()));
        if (debug) MessageBoxW(NULL, L"创建控制窗失败，无法保证单实例与托盘行为", L"DSH Shim 错误", MB_ICONERROR);
        CoUninitialize();
        return 6;
    }
    if (!InstallCloseInterceptors()) {
        LogMessage("ERROR", "关闭语义钩子安装失败：叉号与 Alt+F4 将直接关闭程序（托盘菜单仍可用）");
        MessageBoxW(NULL, L"关闭语义钩子安装失败，叉号与 Alt+F4 会直接关闭程序。\n详见 exe 同级 dsh_pwa_shim.log。",
                    L"DSH Shim 警告", MB_ICONWARNING);
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
        UninstallCloseInterceptors();
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
        UninstallCloseInterceptors();
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
        LogMessage("ERROR", "未能锁定应用窗口 HWND：本次运行没有托盘入口，叉号会直接关闭程序");
    }

    if (targetHwnd != NULL) {
        if (AddTrayIcon()) {
            GlobalState::g_CloseInterceptReady = true;
            LogMessage("INFO", "关闭语义已生效：叉号 / Alt+F4 → 托盘；真关闭 = 右键托盘菜单「退出」");
        } else {
            LogMessage("ERROR", "托盘图标不可用：为免窗口被藏到无法恢复，本次运行不拦截叉号");
        }
    }

    DestroySplashWindow();
    StartConsoleDrainThread(hStdoutRead);
    hStdoutRead = NULL;
    RunEventDrivenLifecyclePump(targetHwnd, targetPid, pi.hProcess);

    LogMessage("INFO", "窗口已销毁，正在强制清理后台进程树 ...");
    RemoveTrayIcon();
    UninstallCloseInterceptors();
    CleanupBackendProcess(hJob, pi.hProcess, hStdoutRead);
    CoUninitialize();
    LogMessage("INFO", "========== DSH Desktop Shim 正常退出 (后台已完全回收) ==========");

    return 0;
}

// ==========================================
// 详细功能与辅助函数实现
// ==========================================

template <typename T>
T LoadUser32(const char* name) {
    T fn = NULL;
    FARPROC raw = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    if (raw != NULL) {
        memcpy(&fn, &raw, sizeof(fn));
    }
    return fn;
}

bool SetupDpiAwareness() {
    PFN_SetProcessDpiAwarenessContext setCtx =
        LoadUser32<PFN_SetProcessDpiAwarenessContext>("SetProcessDpiAwarenessContext");
    PFN_GetDpiForSystem getSystemDpi = LoadUser32<PFN_GetDpiForSystem>("GetDpiForSystem");
    GlobalState::g_GetDpiForWindow = LoadUser32<PFN_GetDpiForWindow>("GetDpiForWindow");
    if (setCtx == NULL || getSystemDpi == NULL || GlobalState::g_GetDpiForWindow == NULL) {
        return false;
    }
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    if (!setCtx((HANDLE)-4)) {
        LogMessage("ERROR", "SetProcessDpiAwarenessContext 调用失败，LastError: " + std::to_string(GetLastError()));
        return false;
    }
    GlobalState::g_SystemDpi = getSystemDpi();
    return true;
}

int DpiScale(int logical) {
    return MulDiv(logical, static_cast<int>(GlobalState::g_SystemDpi), 96);
}

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

// ==========================================
// 单实例：控制窗与第二个实例的前置请求
// ==========================================

bool ActivateExistingInstance() {
    HWND existing = FindWindowW(Config::CONTROLLER_CLASS, Config::CONTROLLER_TITLE);
    if (existing == NULL) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(existing, &pid);
    BOOL granted = AllowSetForegroundWindow(pid);
    if (!PostMessageW(existing, Config::WM_DSH_ACTIVATE, 0, 0)) {
        DWORD err = GetLastError();
        LogMessage("ERROR", "发现已有实例 (PID " + std::to_string(pid) + ") 但前置消息投递失败，LastError: " + std::to_string(err));
        MessageBoxW(NULL,
                    L"DSH 已在运行，但本次无法把它的窗口前置（消息投递失败）。\n"
                    L"请手动切换到已有窗口，或在托盘菜单里退出后重试。",
                    L"DSH Shim", MB_ICONWARNING);
        return true;
    }
    LogMessage("INFO", "检测到已有实例 (PID " + std::to_string(pid) + ")，已请求它前置窗口；本次进程直接退出"
                       "（前台特权转交: " + std::string(granted ? "成功" : "失败") + "）");
    return true;
}

HWND CreateControllerWindow(HINSTANCE hInstance) {
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ControllerWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = Config::CONTROLLER_CLASS;
    if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        Config::CONTROLLER_CLASS,
        Config::CONTROLLER_TITLE,
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );
    if (hwnd == NULL) {
        return NULL;
    }

    GlobalState::g_ControllerHwnd = hwnd;
    GlobalState::g_TaskbarCreatedMsg = RegisterWindowMessageW(Config::TASKBAR_CREATED_MSG);
    return hwnd;
}

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (GlobalState::g_TaskbarCreatedMsg != 0 && msg == GlobalState::g_TaskbarCreatedMsg) {
        LogMessage("INFO", "Explorer 已重启，重建托盘图标");
        if (GlobalState::g_TrayAdded) {
            GlobalState::g_TrayAdded = false;
            AddTrayIcon();
        }
        return 0;
    }

    switch (msg) {
        case Config::WM_DSH_ACTIVATE:
            LogMessage("INFO", "收到第二个实例的前置请求");
            ActivateMainWindow();
            return 0;
        case Config::WM_DSH_HIDDEN:
            LogMessage("INFO", "应用窗口已隐藏到托盘，后台继续运行（重新显示：左键托盘图标或再运行一次 exe）");
            return 0;
        case Config::WM_DSH_TRAY:
            HandleTrayCallback(lParam);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ==========================================
// 关闭语义拦截：低层鼠标 / 键盘钩子 + 托盘
// ==========================================

bool InstallCloseInterceptors() {
    GlobalState::g_MouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GlobalState::g_Instance, 0);
    if (GlobalState::g_MouseHook == NULL) {
        LogMessage("ERROR", "SetWindowsHookEx(WH_MOUSE_LL) 失败，LastError: " + std::to_string(GetLastError()));
        return false;
    }
    GlobalState::g_KeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GlobalState::g_Instance, 0);
    if (GlobalState::g_KeyboardHook == NULL) {
        LogMessage("ERROR", "SetWindowsHookEx(WH_KEYBOARD_LL) 失败，LastError: " + std::to_string(GetLastError()));
        UnhookWindowsHookEx(GlobalState::g_MouseHook);
        GlobalState::g_MouseHook = NULL;
        return false;
    }
    LogMessage("INFO", "低层输入钩子已安装（WH_MOUSE_LL + WH_KEYBOARD_LL）");
    return true;
}

void UninstallCloseInterceptors() {
    if (GlobalState::g_MouseHook != NULL) {
        UnhookWindowsHookEx(GlobalState::g_MouseHook);
        GlobalState::g_MouseHook = NULL;
    }
    if (GlobalState::g_KeyboardHook != NULL) {
        UnhookWindowsHookEx(GlobalState::g_KeyboardHook);
        GlobalState::g_KeyboardHook = NULL;
    }
    GlobalState::g_CloseInterceptReady = false;
}

bool IsCloseIntercepted(const POINT& pt) {
    // 拦截只有在「托盘图标已就绪」时才允许：否则窗口一藏就没有任何入口能把它叫回来。
    if (!GlobalState::g_CloseInterceptReady || GlobalState::g_AppClosed) {
        return false;
    }
    HWND app = GlobalState::g_TargetHwnd;
    if (app == NULL || !IsWindow(app) || !IsWindowVisible(app)) {
        return false;
    }

    RECT rc;
    if (!GetWindowRect(app, &rc) || !PtInRect(&rc, pt)) {
        return false;
    }

    // 只在窗口顶部这条带里跨进程询问命中码：页面内的普通点击不必付这次 IPC 的代价。
    UINT dpi = GlobalState::g_GetDpiForWindow(app);
    if (pt.y >= rc.top + MulDiv(Config::CAPTION_BAND_LOGICAL_PX, static_cast<int>(dpi), 96)) {
        return false;
    }

    // Chromium 自绘标题栏也会如实回答 WM_NCHITTEST：关闭按钮返回 HTCLOSE，标题栏返回 HTCAPTION。
    DWORD_PTR hit = 0;
    if (SendMessageTimeoutW(app, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, Config::HIT_TEST_TIMEOUT_MS, &hit) == 0) {
        LogMessage("WARN", "询问窗口命中码超时，本次点击不拦截");
        return false;
    }
    return (LRESULT)hit == HTCLOSE;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN) {
            MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (IsCloseIntercepted(ms->pt)) {
                GlobalState::g_SwallowButtonUp = true;   // 抬起也要吞，否则会落到别的窗口上
                HideMainWindowToTray();
                return 1;
            }
        } else if (wParam == WM_LBUTTONUP && GlobalState::g_SwallowButtonUp) {
            GlobalState::g_SwallowButtonUp = false;
            return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (IsHideHotkey(kb) && GlobalState::g_CloseInterceptReady && !GlobalState::g_AppClosed &&
            IsAppWindowForeground()) {
            HideMainWindowToTray();
            return 1;   // 吞掉热键，别让 Chromium 走它自己的关闭逻辑
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

bool IsHideHotkey(const KBDLLHOOKSTRUCT* kb) {
    if (kb->vkCode == VK_F4 && (kb->flags & LLKHF_ALTDOWN) != 0) {
        return true;   // Alt+F4
    }
    // Ctrl+Shift+W：低层钩子的 flags 只报 Alt，Ctrl/Shift 要问异步键状态。
    if (kb->vkCode == Config::HIDE_HOTKEY_VK &&
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        return true;
    }
    return false;
}

bool IsAppWindowForeground() {
    HWND app = GlobalState::g_TargetHwnd;
    if (app == NULL || !IsWindow(app) || !IsWindowVisible(app)) {
        return false;
    }
    HWND foreground = GetForegroundWindow();
    return foreground == app || (foreground != NULL && IsChild(app, foreground));
}

void HideMainWindowToTray() {
    HWND app = GlobalState::g_TargetHwnd;
    if (app == NULL || !IsWindow(app) || !IsWindowVisible(app)) {
        return;
    }
    ShowWindow(app, SW_HIDE);
    // 钩子回调里只做最小动作：日志交给控制窗消息处理，避免拖慢输入线程。
    PostMessageW(GlobalState::g_ControllerHwnd, Config::WM_DSH_HIDDEN, 0, 0);
}

void ActivateMainWindow() {
    HWND app = GlobalState::g_TargetHwnd;
    if (app != NULL && IsWindow(app)) {
        ShowWindow(app, IsIconic(app) ? SW_RESTORE : SW_SHOW);
        if (!SetForegroundWindow(app)) {
            LogMessage("WARN", "SetForegroundWindow 失败，窗口可能仍在其它窗口之后");
        }
        BringWindowToTop(app);
        LogMessage("INFO", "已前置应用窗口");
        return;
    }

    HWND splash = GlobalState::g_SplashHwnd;
    if (splash != NULL && IsWindow(splash)) {
        SetForegroundWindow(splash);
        LogMessage("INFO", "应用窗口尚未就绪，已前置启动窗");
    }
}

void HandleTrayCallback(LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ActivateMainWindow();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        default:
            break;
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == NULL) {
        LogMessage("ERROR", "CreatePopupMenu 失败，LastError: " + std::to_string(GetLastError()));
        return;
    }
    AppendMenuW(menu, MF_STRING, Config::TRAY_MENU_SHOW, L"显示窗口(&S)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, Config::TRAY_MENU_QUIT, L"退出并结束后台(&X)");
    SetMenuDefaultItem(menu, Config::TRAY_MENU_SHOW, FALSE);

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(GlobalState::g_ControllerHwnd);   // 托盘菜单的经典要求：先抢前台，菜单才会正常消失
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              cursor.x, cursor.y, 0, GlobalState::g_ControllerHwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(GlobalState::g_ControllerHwnd, WM_NULL, 0, 0);

    if (cmd == Config::TRAY_MENU_SHOW) {
        ActivateMainWindow();
    } else if (cmd == Config::TRAY_MENU_QUIT) {
        RequestQuit();
    }
}

void RequestQuit() {
    LogMessage("INFO", "托盘菜单请求退出：关闭应用窗口并回收后台");
    HWND app = GlobalState::g_TargetHwnd;
    if (app != NULL && IsWindow(app)) {
        PostMessageW(app, WM_CLOSE, 0, 0);
    }
    PostQuitMessage(0);   // 不依赖窗口是否响应，回收路径固定走主循环出口
}

bool AddTrayIcon() {
    if (GlobalState::g_ControllerHwnd == NULL) {
        return false;
    }

    if (GlobalState::g_TrayIcon == NULL) {
        int size = GetSystemMetrics(SM_CXSMICON);
        GlobalState::g_TrayIcon = reinterpret_cast<HICON>(LoadImageW(
            GlobalState::g_Instance,
            MAKEINTRESOURCEW(Config::IDI_DSH_TRAY),
            IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (GlobalState::g_TrayIcon == NULL) {
            LogMessage("ERROR", "内嵌托盘图标加载失败 (IDI_DSH_TRAY)，LastError: " + std::to_string(GetLastError()));
            return false;
        }
    }

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = GlobalState::g_ControllerHwnd;
    nid.uID = Config::TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = Config::WM_DSH_TRAY;
    nid.hIcon = GlobalState::g_TrayIcon;
    lstrcpynW(nid.szTip, Config::TRAY_TIP, ARRAYSIZE(nid.szTip));

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        LogMessage("ERROR", "Shell_NotifyIcon(NIM_ADD) 失败，LastError: " + std::to_string(GetLastError()));
        return false;
    }
    GlobalState::g_TrayAdded = true;
    LogMessage("INFO", "托盘图标已添加（左键显示窗口，右键菜单可退出）");
    return true;
}

void RemoveTrayIcon() {
    if (GlobalState::g_TrayAdded && GlobalState::g_ControllerHwnd != NULL) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = GlobalState::g_ControllerHwnd;
        nid.uID = Config::TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        GlobalState::g_TrayAdded = false;
        LogMessage("INFO", "托盘图标已移除");
    }
    if (GlobalState::g_TrayIcon != NULL) {
        DestroyIcon(GlobalState::g_TrayIcon);
        GlobalState::g_TrayIcon = NULL;
    }
}

// ==========================================
// 后台进程：启动、管道与 Job Object
// ==========================================

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

// ==========================================
// 启动窗与 Edge 应用窗口
// ==========================================

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

            HFONT oldFont = NULL;
            if (GlobalState::g_SplashFont != NULL) {
                oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, GlobalState::g_SplashFont));
            }

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            RECT titleRc = rc;
            titleRc.top += DpiScale(56);
            titleRc.bottom = titleRc.top + DpiScale(40);
            DrawTextW(hdc, L"DeepSeek Harness", -1, &titleRc, DT_CENTER | DT_SINGLELINE);

            SetTextColor(hdc, RGB(170, 170, 170));
            RECT statusRc = rc;
            statusRc.top = titleRc.bottom + DpiScale(12);
            DrawTextW(hdc, GlobalState::g_SplashStatus, -1, &statusRc, DT_CENTER | DT_SINGLELINE);

            if (oldFont != NULL) {
                SelectObject(hdc, oldFont);
            }
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
    lstrcpynW(GlobalState::g_SplashStatus, L"正在启动 ...", ARRAYSIZE(GlobalState::g_SplashStatus));
    GlobalState::g_SplashCancelled = false;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = SplashWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = Config::SPLASH_CLASS;
    RegisterClassW(&wc);

    if (GlobalState::g_SplashFont == NULL) {
        GlobalState::g_SplashFont = CreateFontW(
            -DpiScale(Config::SPLASH_FONT_HEIGHT_LOGICAL), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    int width = DpiScale(Config::SPLASH_WIDTH_LOGICAL);
    int height = DpiScale(Config::SPLASH_HEIGHT_LOGICAL);
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_TOPMOST,
        Config::SPLASH_CLASS,
        L"DeepSeek Harness",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height,
        NULL, NULL, hInstance, NULL
    );
    if (hwnd == NULL) return NULL;

    GlobalState::g_SplashHwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

void SetSplashStatus(const wchar_t* status) {
    lstrcpynW(GlobalState::g_SplashStatus, status ? status : L"", ARRAYSIZE(GlobalState::g_SplashStatus));
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
    if (GlobalState::g_SplashFont != NULL) {
        DeleteObject(GlobalState::g_SplashFont);
        GlobalState::g_SplashFont = NULL;
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
