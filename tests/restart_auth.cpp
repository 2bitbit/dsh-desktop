/** @file restart_auth.cpp @brief 隔离验证市场重启认证，不启动浏览器或真实 DSH。 */
#include "../src/dsh_desktop.cpp"
#include <cstdlib>

namespace RestartTest {

/** 失败用例数，决定测试进程的退出码。 */
int failures = 0;
/** Node 可执行文件、HTTP 夹具与本次独立数据目录。 */
std::wstring nodePath, fixturePath, dataRoot;
/** 目录序号，确保每个用例只看到自己的日志。 */
int directoryIndex = 0;

/** 输出用例结果，不打印 URL、令牌或 Cookie。 */
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "通过" : "失败", name);
    if (!condition) ++failures;
}

/** 为单个用例创建独立目录，并切换当前测试进程的临时目录。 */
std::wstring CaseDirectory(const std::wstring& name) {
    std::wstring directory = dataRoot + L"\\" + std::to_wstring(++directoryIndex) + L"-" + name;
    if (!CreateDirectoryW(directory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        throw std::string("无法创建隔离测试目录");
    }
    if (!SetEnvironmentVariableW(L"TEMP", directory.c_str()) ||
        !SetEnvironmentVariableW(L"TMP", directory.c_str())) {
        throw std::string("无法设置测试进程临时目录");
    }
    return directory;
}

/** 从测试目录读取一个短文本文件，缺失时返回空字符串。 */
std::string ReadSmallFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return {};
    char buffer[128];
    DWORD read = 0;
    BOOL ok = ReadFile(file, buffer, sizeof(buffer), &read, NULL);
    CloseHandle(file);
    return ok ? std::string(buffer, read) : std::string();
}

/** 构造只指向测试回环端口的认证 URL。 */
std::string AuthUrl(int port, const std::string& token = "fixture-current") {
    return "http://127.0.0.1:" + std::to_string(port) + "/?token=" + token;
}

/** 判断读取结果是否包含目标，不输出目标内容。 */
bool Contains(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

/** 追加测试日志，保持写句柄由调用者负责关闭。 */
void Append(HANDLE file, const std::string& text) {
    DWORD written = 0;
    if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, NULL) ||
        written != text.size()) throw std::string("无法写入测试日志");
}

/** 只创建和回收本用例的子进程，不访问任何用户进程。 */
class Fixture {
public:
    /** 测试子进程信息、自有 Job、目录和动态监听端口。 */
    PROCESS_INFORMATION process{};
    HANDLE job = NULL;
    std::wstring directory;
    int port = 0;

    /** 启动暂停的 Node 夹具，按需绑定自有 Job，等待端口文件。 */
    Fixture(const std::wstring& mode, bool owned) {
        directory = CaseDirectory(mode);
        job = InitializeJobObject();
        if (job == NULL) throw std::string("无法创建测试 Job");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
        HANDLE sink = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (sink == INVALID_HANDLE_VALUE) {
            CloseHandle(job);
            throw std::string("无法打开测试 NUL 句柄");
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = startup.hStdOutput = startup.hStdError = sink;
        std::wstring command = L"\"" + nodePath + L"\" \"" +
                               fixturePath + L"\" \"" + directory + L"\" " + mode;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        BOOL created = CreateProcessW(nodePath.c_str(), mutableCommand.data(), NULL, NULL, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                                      directory.c_str(), &startup, &process);
        CloseHandle(sink);
        if (!created) {
            CloseHandle(job);
            throw std::string("无法启动测试夹具");
        }
        if (owned && !AssignProcessToJobObject(job, process.hProcess)) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 3000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
            throw std::string("无法将测试夹具绑定到自有 Job");
        }
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        process.hThread = NULL;
        const ULONGLONG deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline) {
            port = std::atoi(ReadSmallFile(directory + L"\\port.txt").c_str());
            if (port > 0) return;
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(20);
        }
        Stop();
        throw std::string("测试夹具未能报告监听端口");
    }

    /** 夹具不可复制，避免重复回收子进程。 */
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    /** 离开用例时只终止本构造器创建的子进程。 */
    ~Fixture() { Stop(); }

    /** 安全回收本夹具的句柄与进程，允许重复调用。 */
    void Stop() {
        if (process.hProcess != NULL) {
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 3000);
            CloseHandle(process.hProcess);
            process.hProcess = NULL;
        }
        if (job != NULL) {
            CloseHandle(job);
            job = NULL;
        }
    }
};

/** 建立监听监督所需的全局状态，旧后台在用例中不存在。 */
void SetSupervisor(Fixture& fixture) {
    GlobalState::g_Job = fixture.job;
    GlobalState::g_BackendProcess = NULL;
    GlobalState::g_ServerPort = fixture.port;
    GlobalState::g_AppClosed = false;
    GlobalState::g_Restarting = false;
}

/** 解除测试全局引用，避免夹具回收后留下悬空句柄。 */
void ClearSupervisor() {
    GlobalState::g_Job = NULL;
    GlobalState::g_BackendProcess = NULL;
    GlobalState::g_ServerPort = 0;
}

/** 回归旧 mtime 与持续打开的写句柄，不以属性时间淘汰当前内容。 */
void TestOpenWriter() {
    std::wstring directory = CaseDirectory(L"open-writer");
    std::wstring path = directory + L"\\dsh-market-restart-open.out.log";
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (writer == INVALID_HANDLE_VALUE) throw std::string("无法创建持久日志写句柄");
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER old{};
    old.LowPart = now.dwLowDateTime;
    old.HighPart = now.dwHighDateTime;
    old.QuadPart -= 60ULL * 10000000ULL;
    FILETIME earlier{old.LowPart, old.HighPart};
    Append(writer, "dsh web: " + AuthUrl(31001) + "\n");
    Check(SetFileTime(writer, NULL, NULL, &earlier) != FALSE, "设置隔离日志旧 mtime");
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(path.c_str(), &found);
    Check(search != INVALID_HANDLE_VALUE && CompareFileTime(&found.ftLastWriteTime, &now) < 0,
          "枚举可见 mtime 早于判定时刻");
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    Check(Contains(ReadMarketRestartAuthUrls(31001), AuthUrl(31001)),
          "旧 mtime 且写句柄未关闭仍能读取完整 URL");
    CloseHandle(writer);
}

/** 完整换行前不接受分块 URL，并拒绝非回环、异端口与畸形候选。 */
void TestCompleteLines() {
    std::wstring directory = CaseDirectory(L"split-lines");
    HANDLE writer = CreateFileW((directory + L"\\dsh-market-restart-split.out.log").c_str(),
                               GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (writer == INVALID_HANDLE_VALUE) throw std::string("无法创建分块日志");
    Append(writer, "dsh web: http://127.0.0.1:31002/?token=");
    Check(ReadMarketRestartAuthUrls(31002).empty(), "半行 URL 不进入候选");
    Append(writer, "fixture-current");
    Check(ReadMarketRestartAuthUrls(31002).empty(), "无结束换行的完整 URL 仍不进入候选");
    Append(writer, "\r\n");
    Append(writer, "dsh web: " + AuthUrl(31002) + "\n");
    Append(writer, "dsh web: http://localhost:31002/?token=fixture-current\n");
    Append(writer, "dsh web: http://127.0.0.1:31003/?token=fixture-current\n");
    Append(writer, "dsh web: http://127.0.0.1:31002/?token=bad+token\n");
    Append(writer, "dsh web: http://127.0.0.1:31002/other?token=fixture-current\n");
    auto urls = ReadMarketRestartAuthUrls(31002);
    Check(urls.size() == 1 && Contains(urls, AuthUrl(31002)),
          "完整行去重且过滤错误 authority、端口、路径及令牌字符");
    CloseHandle(writer);
}

/** 日志超过读取上限时仍查尾部，不误用文件开头的旧令牌。 */
void TestLogTail() {
    std::wstring directory = CaseDirectory(L"log-tail");
    HANDLE writer = CreateFileW((directory + L"\\dsh-market-restart-tail.out.log").c_str(),
                               GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (writer == INVALID_HANDLE_VALUE) throw std::string("无法创建尾部读取日志");
    Append(writer, "dsh web: " + AuthUrl(31004, "fixture-stale") + "\n");
    Append(writer, std::string(300 * 1024, 'x') + "\n");
    Append(writer, "dsh web: " + AuthUrl(31004) + "\n");
    auto urls = ReadMarketRestartAuthUrls(31004);
    Check(urls.size() == 1 && Contains(urls, AuthUrl(31004)), "大日志读取尾部且不接受截断行或旧头部");
    CloseHandle(writer);

    // 尾部窗口必须覆盖“offset 正好落在完整行起点”的边界。
    std::wstring alignedPath = directory + L"\\dsh-market-restart-aligned.out.log";
    HANDLE aligned = CreateFileW(alignedPath.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (aligned == INVALID_HANDLE_VALUE) throw std::string("无法创建对齐边界日志");
    const std::string alignedUrl = "dsh web: " + AuthUrl(31005) + "\n";
    const size_t tailBytes = Config::MAX_RESTART_LOG_BYTES;
    std::string prefix(15, 'x');
    prefix.back() = '\n';
    const size_t fillerBytes = tailBytes - alignedUrl.size() - 1;
    std::string suffix(fillerBytes, 'x');
    Append(aligned, prefix);
    Append(aligned, alignedUrl);
    Append(aligned, "\n");
    Append(aligned, suffix);
    Check(Contains(ReadMarketRestartAuthUrls(31005), AuthUrl(31005)),
          "日志尾部从完整行起点开始时不丢认证 URL");
    CloseHandle(aligned);
}

/** 验证兑换合同与首页，不把恢复页面或普通 HTTP 200 当作鉴权成功。 */
void TestHttpContract() {
    Fixture fixture(L"valid", true);
    DWORD status = 0;
    Check(VerifyAuthUrl(AuthUrl(fixture.port), fixture.port, status),
          "303 兑换 Cookie 后首页 200 且含启动标记");
    status = 0;
    Check(!VerifyAuthUrl(AuthUrl(fixture.port, "fixture-stale"), fixture.port, status) && status == 401,
          "陈旧令牌 HTTP 401 被拒绝");
    Check(!VerifyAuthUrl("http://localhost:" + std::to_string(fixture.port) +
                        "/?token=fixture-current", fixture.port, status), "非规范回环 URL 被拒绝");
    Check(!VerifyAuthUrl(AuthUrl(fixture.port), fixture.port + 1, status), "不同端口 URL 被拒绝");
    Check(!VerifyAuthUrl("http://example.invalid/?token=fixture-current", fixture.port, status),
          "外部 URL 在网络请求前被拒绝");
    Check(!VerifyAuthUrl(AuthUrl(fixture.port) + "&extra=1", fixture.port, status),
          "附加查询参数被拒绝");
}

/** 每个失败响应模式使用独立服务器和目录。 */
void TestRejectedContract(const std::wstring& mode, const char* name) {
    Fixture fixture(mode, true);
    DWORD status = 0;
    Check(!VerifyAuthUrl(AuthUrl(fixture.port), fixture.port, status), name);
}

/** 只读取菜单项，不显示真实 UI；灰显状态不能修改重启命令 ID。 */
void TestTrayMenu() {
    GlobalState::g_Restarting = false;
    HMENU available = CreateTrayMenu();
    Check(available != NULL && GetMenuItemID(available, 1) == Config::TRAY_MENU_RESTART &&
          (GetMenuState(available, Config::TRAY_MENU_RESTART, MF_BYCOMMAND) & MF_GRAYED) == 0,
          "未重启时菜单 ID 保持重启且可用");
    if (available != NULL) DestroyMenu(available);
    GlobalState::g_Restarting = true;
    HMENU disabled = CreateTrayMenu();
    Check(disabled != NULL && GetMenuItemID(disabled, 1) == Config::TRAY_MENU_RESTART &&
          (GetMenuState(disabled, Config::TRAY_MENU_RESTART, MF_BYCOMMAND) & MF_GRAYED) == MF_GRAYED,
          "重启中菜单 ID 仍为重启且正确灰显");
    if (disabled != NULL) DestroyMenu(disabled);
    GlobalState::g_Restarting = false;
}

/** 不响应的测试服务器应触发有限 HTTP 预算，不声称硬实时截止。 */
void TestSlowHttp() {
    Fixture fixture(L"slow", true);
    DWORD status = 0;
    const ULONGLONG start = GetTickCount64();
    const bool accepted = VerifyAuthUrl(AuthUrl(fixture.port), fixture.port, status, 300);
    const ULONGLONG elapsed = GetTickCount64() - start;
    std::printf("[诊断] 慢 HTTP accepted=%d status=%lu elapsed=%llu ms\n",
                accepted ? 1 : 0, static_cast<unsigned long>(status), elapsed);
    Check(!accepted && elapsed < 5000, "慢 HTTP 兑换按有限预算拒绝（同步 WinHTTP 的软上限）");
}

/** 监听先发生，分块日志最后换行后才允许接管本 Job 的宿主。 */
void TestDelayedRestart() {
    Fixture fixture(L"delayed", true);
    SetSupervisor(fixture);
    Check(FindPortListenerPid(fixture.port) == fixture.process.dwProcessId, "夹具先监听端口");
    const ULONGLONG start = GetTickCount64();
    std::string url;
    HANDLE restarted = WaitForBackendRestart(fixture.port, 7000, url);
    Check(restarted != NULL && GetProcessId(restarted) == fixture.process.dwProcessId &&
          url == AuthUrl(fixture.port) && GetTickCount64() - start >= 1700,
          "等待延迟日志完整换行及 HTTP 夹具兑换后才接管");
    if (restarted != NULL) CloseHandle(restarted);
    Check(ReadSmallFile(fixture.directory + L"\\stale-count.txt") == "1",
          "同一监听 PID 的旧令牌 401 只验证一次");
    ClearSupervisor();
}

/** 同一端口的外部监听者不属于指定 Job，必须拒绝接管。 */
void TestExternalListener() {
    Fixture fixture(L"valid", false);
    SetSupervisor(fixture);
    std::string url;
    HANDLE restarted = WaitForBackendRestart(fixture.port, 700, url);
    Check(restarted == NULL && url.empty(), "拒绝接管指定 Job 外的监听进程");
    Check(WaitForSingleObject(fixture.process.hProcess, 0) == WAIT_TIMEOUT,
          "拒绝接管不会终止外部测试进程");
    if (restarted != NULL) CloseHandle(restarted);
    ClearSupervisor();
}

/** 已有后台 PID 不能被误认成新宿主。 */
void TestOldListener() {
    Fixture fixture(L"valid", true);
    SetSupervisor(fixture);
    GlobalState::g_BackendProcess = fixture.process.hProcess;
    std::string url;
    HANDLE restarted = WaitForBackendRestart(fixture.port, 600, url);
    Check(restarted == NULL && url.empty(), "旧后台 PID 即使鉴权有效也不接管");
    if (restarted != NULL) CloseHandle(restarted);
    ClearSupervisor();
}

/** 无 URL 分支按预算结束；不据此声称慢 HTTP 分支达到硬实时截止。 */
void TestTimeout() {
    Fixture fixture(L"silent", true);
    SetSupervisor(fixture);
    const ULONGLONG start = GetTickCount64();
    std::string url;
    HANDLE restarted = WaitForBackendRestart(fixture.port, 650, url);
    const ULONGLONG elapsed = GetTickCount64() - start;
    Check(restarted == NULL && url.empty() && elapsed >= 600 && elapsed < 1800,
          "无认证日志按有限预算超时");
    if (restarted != NULL) CloseHandle(restarted);
    ClearSupervisor();
}

/** 执行全部测试，只测试替身 HTTP 合同，不声称真实 DSH 端到端通过。 */
int Run(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "需要 Node 路径、夹具路径、隔离数据目录\n");
        return 2;
    }
    nodePath = FromUtf8(argv[1]);
    fixturePath = FromUtf8(argv[2]);
    dataRoot = FromUtf8(argv[3]);
    try {
        TestOpenWriter();
        TestCompleteLines();
        TestLogTail();
        TestHttpContract();
        TestRejectedContract(L"recovery", "HTTP 恢复页面假 200 被拒绝");
        TestRejectedContract(L"server-error", "兑换响应 HTTP 500 被拒绝");
        TestRejectedContract(L"no-cookie", "303 未签发兑换 Cookie 被拒绝");
        TestRejectedContract(L"foreign-location", "303 外部跳转被拒绝");
        TestRejectedContract(L"no-boot", "已兑换但首页没有启动标记被拒绝");
        TestRejectedContract(L"wrong-content", "启动标记出现在非 HTML 响应仍拒绝");
        TestTrayMenu();
        TestSlowHttp();
        TestDelayedRestart();
        TestExternalListener();
        TestOldListener();
        TestTimeout();
    } catch (const std::string& error) {
        ClearSupervisor();
        std::fprintf(stderr, "[失败] %s\n", error.c_str());
        ++failures;
    }
    std::printf("隔离回归结束：失败 %d 项；未启动真实 DSH 或浏览器。\n", failures);
    return failures == 0 ? 0 : 1;
}
} // namespace RestartTest

/** 控制台测试入口，不调用启动器的 wWinMain。 */
int main(int argc, char** argv) { return RestartTest::Run(argc, argv); }
