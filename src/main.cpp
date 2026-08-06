/**
 * 文件作用：Codex Status Light 命令行入口
 * 职责范围：
 * 1. 解析 P0 取证、P1 状态和 P2 托盘模式参数
 * 2. 启动 StatusLight.exe --inspect、--status 或默认托盘模式
 * 3. 输出清晰的用法和错误信息
 *
 * 不负责：
 * - 托盘窗口和图标渲染
 * - JSONL 事件语义实现细节
 *
 * 维护说明：
 * - 后续 P2 托盘入口应在这里分流，不要把 UI 逻辑塞进 inspect 命令
 */
#include "InspectCommand.h"
#include "StatusCommand.h"
#include "TrayApp.h"
#include "web/NativeMessagingHost.h"
#include "web/WebDebugLog.h"
#include "web/WebSelfTest.h"

#include <Windows.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

void PrintUsage()
{
    std::cout << "Usage:\n";
    std::cout << "  StatusLight.exe\n";
    std::cout << "  StatusLight.exe --tray [--codex-home <path>] [--max-files <count>] [--recent-hours <hours>] [--poll-seconds <seconds>]\n";
    std::cout << "  StatusLight.exe --status [--codex-home <path>] [--max-files <count>] [--recent-hours <hours>] [--watch] [--poll-seconds <seconds>]\n";
    std::cout << "  StatusLight.exe --inspect [--codex-home <path>] [--max-files <count>] [--recent-hours <hours>] [--watch] [--poll-seconds <seconds>]\n";
    std::cout << "  StatusLight.exe --self-test-web\n";
    std::cout << "  StatusLight.exe --web-log-path\n";
}

bool ReadSizeArgument(int argc, wchar_t* argv[], int* index, size_t* output)
{
    if (*index + 1 >= argc) {
        return false;
    }

    try {
        *output = static_cast<size_t>(std::stoul(argv[*index + 1]));
    } catch (...) {
        return false;
    }

    ++(*index);
    return true;
}

bool ReadIntArgument(int argc, wchar_t* argv[], int* index, int* output)
{
    if (*index + 1 >= argc) {
        return false;
    }

    try {
        *output = std::stoi(argv[*index + 1]);
    } catch (...) {
        return false;
    }

    ++(*index);
    return true;
}

void AttachConsoleForCommandLineMode()
{
    // P4 使用 Windows 子系统避免双击时出现控制台；调试命令需要主动附加父控制台输出文本。
    const HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (outputHandle != nullptr &&
        outputHandle != INVALID_HANDLE_VALUE &&
        GetFileType(outputHandle) != FILE_TYPE_UNKNOWN) {
        std::ios::sync_with_stdio(true);
        std::cout.clear();
        std::cerr.clear();
        std::wcout.clear();
        std::wcerr.clear();
        return;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }

    FILE* ignoredStream = nullptr;
    freopen_s(&ignoredStream, "CONOUT$", "w", stdout);
    freopen_s(&ignoredStream, "CONOUT$", "w", stderr);
    freopen_s(&ignoredStream, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::wcout.clear();
    std::wcerr.clear();
}

void ConfigureDpiAwareness()
{
#ifdef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
#endif
    SetProcessDPIAware();
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    ConfigureDpiAwareness();

    if (NativeMessagingHost::LooksLikeNativeHostInvocation(argc, argv)) {
        NativeMessagingHost host;
        return host.Run(argc, argv);
    }

    // 无参数启动通常来自双击 EXE。P2 默认进入托盘模式，控制台命令仍保留用于调试。
    bool inspectMode = false;
    bool selfTestWebMode = false;
    bool webLogPathMode = false;
    bool statusMode = false;
    bool trayMode = argc == 1;
    InspectOptions inspectOptions;
    StatusOptions statusOptions;
    TrayOptions trayOptions;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--inspect") {
            inspectMode = true;
            selfTestWebMode = false;
            statusMode = false;
            trayMode = false;
            continue;
        }

        if (argument == L"--self-test-web") {
            selfTestWebMode = true;
            inspectMode = false;
            webLogPathMode = false;
            statusMode = false;
            trayMode = false;
            continue;
        }

        if (argument == L"--web-log-path") {
            webLogPathMode = true;
            inspectMode = false;
            selfTestWebMode = false;
            statusMode = false;
            trayMode = false;
            continue;
        }

        if (argument == L"--status") {
            statusMode = true;
            inspectMode = false;
            selfTestWebMode = false;
            webLogPathMode = false;
            trayMode = false;
            continue;
        }

        if (argument == L"--tray") {
            trayMode = true;
            statusMode = false;
            inspectMode = false;
            selfTestWebMode = false;
            webLogPathMode = false;
            continue;
        }

        if (argument == L"--codex-home") {
            if (index + 1 >= argc) {
                AttachConsoleForCommandLineMode();
                std::cerr << "--codex-home requires a path\n";
                return 2;
            }
            const std::wstring codexHome = argv[++index];
            inspectOptions.codexHome = codexHome;
            statusOptions.codexHome = codexHome;
            trayOptions.codexHome = codexHome;
            continue;
        }

        if (argument == L"--max-files") {
            size_t maxFiles = 0;
            if (!ReadSizeArgument(argc, argv, &index, &maxFiles)) {
                AttachConsoleForCommandLineMode();
                std::cerr << "--max-files requires a positive integer\n";
                return 2;
            }
            inspectOptions.maxFiles = maxFiles;
            statusOptions.maxFiles = maxFiles;
            trayOptions.maxFiles = maxFiles;
            continue;
        }

        if (argument == L"--recent-hours") {
            int recentHours = 0;
            if (!ReadIntArgument(argc, argv, &index, &recentHours)) {
                AttachConsoleForCommandLineMode();
                std::cerr << "--recent-hours requires a positive integer\n";
                return 2;
            }
            inspectOptions.recentHours = recentHours;
            statusOptions.recentHours = recentHours;
            trayOptions.recentHours = recentHours;
            continue;
        }

        if (argument == L"--watch") {
            inspectOptions.watch = true;
            statusOptions.watch = true;
            continue;
        }

        if (argument == L"--poll-seconds") {
            int pollSeconds = 0;
            if (!ReadIntArgument(argc, argv, &index, &pollSeconds)) {
                AttachConsoleForCommandLineMode();
                std::cerr << "--poll-seconds requires a positive integer\n";
                return 2;
            }
            inspectOptions.pollSeconds = pollSeconds;
            statusOptions.pollSeconds = pollSeconds;
            trayOptions.pollSeconds = pollSeconds;
            continue;
        }

        AttachConsoleForCommandLineMode();
        std::wcerr << L"unknown argument: " << argument << L"\n";
        PrintUsage();
        return 2;
    }

    if (!inspectMode && !selfTestWebMode && !webLogPathMode && !statusMode && !trayMode) {
        AttachConsoleForCommandLineMode();
        PrintUsage();
        return 0;
    }

    if (inspectMode || selfTestWebMode || webLogPathMode || statusMode) {
        AttachConsoleForCommandLineMode();
    }

    if (inspectOptions.maxFiles == 0 || inspectOptions.recentHours <= 0 || inspectOptions.pollSeconds <= 0 ||
        statusOptions.maxFiles == 0 || statusOptions.recentHours <= 0 || statusOptions.pollSeconds <= 0 ||
        trayOptions.maxFiles == 0 || trayOptions.recentHours <= 0 || trayOptions.pollSeconds <= 0) {
        AttachConsoleForCommandLineMode();
        std::cerr << "--max-files, --recent-hours, and --poll-seconds must be positive\n";
        return 2;
    }

    HANDLE singleInstanceMutex = nullptr;
    if (trayMode) {
        singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\CodexStatusLightSingleInstance");
        if (singleInstanceMutex == nullptr) {
            AttachConsoleForCommandLineMode();
            std::cerr << "failed to create single instance mutex\n";
            return 2;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(singleInstanceMutex);
            return 0;
        }
    }

    if (inspectMode) {
        InspectCommand command;
        return command.Run(inspectOptions);
    }

    if (selfTestWebMode) {
        WebSelfTest command;
        return command.Run();
    }

    if (webLogPathMode) {
        std::wcout << WebDebugLog::LogPath() << L"\n";
        return 0;
    }

    if (statusMode) {
        StatusCommand command;
        return command.Run(statusOptions);
    }

    TrayApp app;
    const int exitCode = app.Run(GetModuleHandleW(nullptr), trayOptions);
    if (singleInstanceMutex != nullptr) {
        CloseHandle(singleInstanceMutex);
    }
    return exitCode;
}
