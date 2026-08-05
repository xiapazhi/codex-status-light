/**
 * 文件作用：实现 P3 Win32 托盘应用
 * 职责范围：
 * 1. 通过 Shell_NotifyIconW 显示任务状态、额度环和异常角标
 * 2. 接入 ReadDirectoryChangesW 监听、5 秒校准扫描和监听恢复
 * 3. 接入 Codex 进程检测，用于自动退出和诊断信息
 *
 * 不负责：
 * - JSONL 事件语义解析
 * - 根据进程存在推断任务正在运行
 *
 * 维护说明：
 * - 托盘层只消费 StatusReader 的状态快照，任务状态判断不能散落到 UI 代码之外
 */
#include "TrayApp.h"

#include <Shellapi.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSourceChangedMessage = WM_APP + 2;
constexpr UINT_PTR kRefreshTimer = 1001;
constexpr UINT_PTR kBlinkTimer = 1002;
constexpr UINT kMenuOpenCodex = 2001;
constexpr UINT kMenuClearCompleted = 2002;
constexpr UINT kMenuCopyDiagnostics = 2003;
constexpr UINT kMenuExit = 2004;
constexpr ULONGLONG kCalibrationIntervalMs = 5000;
constexpr ULONGLONG kStartupNoCodexGraceMs = 15000;
constexpr ULONGLONG kRuntimeNoCodexGraceMs = 5000;
constexpr UINT kAnimationTimerMs = 125;
constexpr ULONGLONG kRunningBreathPeriodMs = 2400;
constexpr ULONGLONG kFastFlashIntervalMs = 125;
constexpr ULONGLONG kRedFlashDurationMs = 3000;
constexpr ULONGLONG kGreenFlashDurationMs = 1000;

} // namespace

int TrayApp::Run(HINSTANCE instance, const TrayOptions& options)
{
    options_ = options;
    startedAtTick_ = GetTickCount64();

    if (!CreateHiddenWindow(instance)) {
        return 2;
    }

    RefreshState();
    StartOrRecoverWatcher();

    if (!AddTrayIcon()) {
        DestroyWindow(hwnd_);
        return 2;
    }

    SetTimer(hwnd_, kRefreshTimer, static_cast<UINT>(std::max(1, options_.pollSeconds) * 1000), nullptr);
    SetTimer(hwnd_, kBlinkTimer, kAnimationTimerMs, nullptr);

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    RemoveTrayIcon();
    watcher_.Stop();
    return 0;
}

LRESULT CALLBACK TrayApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    TrayApp* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<TrayApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            app->processSnapshot_ = app->processMonitor_.ReadOnce();
            const ULONGLONG nowTick = GetTickCount64();
            const bool needsCalibration = nowTick - app->lastCalibrationTick_ >= kCalibrationIntervalMs;
            const bool hasFileChange = app->watcher_.ConsumeChangeSignal();

            if (hasFileChange || needsCalibration) {
                app->RefreshState();
                app->StartOrRecoverWatcher();
            }

            if (app->ShouldAutoExit(app->AggregateSessions())) {
                DestroyWindow(hwnd);
            }
        } else if (wParam == kBlinkTimer) {
            app->blinkOn_ = !app->blinkOn_;
            app->UpdateTrayIcon();
        }
        return 0;
    case kSourceChangedMessage:
        app->RefreshState();
        app->StartOrRecoverWatcher();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuOpenCodex:
            app->OpenCodex();
            return 0;
        case kMenuClearCompleted:
            app->ClearCompletedPrompts();
            return 0;
        case kMenuCopyDiagnostics:
            app->CopyDiagnosticsToClipboard();
            return 0;
        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case kTrayMessage:
        app->HandleTrayMessage(lParam);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        KillTimer(hwnd, kBlinkTimer);
        app->RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool TrayApp::CreateHiddenWindow(HINSTANCE instance)
{
    const wchar_t* className = L"CodexStatusLightTrayWindow";

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TrayApp::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(
        0,
        className,
        L"Codex Status Light",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        this);

    return hwnd_ != nullptr;
}

bool TrayApp::AddTrayIcon()
{
    notifyData_ = {};
    notifyData_.cbSize = sizeof(notifyData_);
    notifyData_.hWnd = hwnd_;
    notifyData_.uID = 1;
    notifyData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notifyData_.uCallbackMessage = kTrayMessage;

    const AggregateSnapshot aggregate = AggregateSessions();
    UpdateVisualTiming(aggregate.visual);
    notifyData_.hIcon = iconRenderer_.GetIcon(BuildIconKey(aggregate));

    const std::wstring tooltip = BuildTooltip(aggregate);
    wcsncpy_s(notifyData_.szTip, _countof(notifyData_.szTip), tooltip.c_str(), _TRUNCATE);

    return Shell_NotifyIconW(NIM_ADD, &notifyData_) != FALSE;
}

void TrayApp::RemoveTrayIcon()
{
    if (notifyData_.hWnd != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &notifyData_);
        notifyData_.hWnd = nullptr;
    }
}

void TrayApp::RefreshState()
{
    lastCalibrationTick_ = GetTickCount64();
    processSnapshot_ = processMonitor_.ReadOnce();

    StatusReadOptions readOptions;
    readOptions.codexHome = options_.codexHome;
    readOptions.maxFiles = options_.maxFiles;
    readOptions.recentHours = options_.recentHours;
    snapshot_ = reader_.ReadOnce(readOptions);

    for (const auto& item : snapshot_.sessions) {
        if (item.second.state != TaskState::Completed) {
            acknowledgedCompletedSessions_.erase(item.first);
        }
    }

    UpdateTrayIcon();
}

void TrayApp::StartOrRecoverWatcher()
{
    if (snapshot_.sessionsPath.empty()) {
        return;
    }
    if (watcher_.IsRunning()) {
        return;
    }

    const bool started = watcher_.Start(snapshot_.sessionsPath, hwnd_, kSourceChangedMessage);
    if (!started) {
        snapshot_.hasSourceError = true;
        snapshot_.errorMessage = watcher_.LastError();
        UpdateTrayIcon();
    }
}

void TrayApp::UpdateTrayIcon()
{
    if (notifyData_.hWnd == nullptr) {
        return;
    }

    const AggregateSnapshot aggregate = AggregateSessions();
    UpdateVisualTiming(aggregate.visual);
    notifyData_.hIcon = iconRenderer_.GetIcon(BuildIconKey(aggregate));

    const std::wstring tooltip = BuildTooltip(aggregate);
    wcsncpy_s(notifyData_.szTip, _countof(notifyData_.szTip), tooltip.c_str(), _TRUNCATE);
    notifyData_.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &notifyData_);
}

void TrayApp::UpdateVisualTiming(AggregateVisual visual)
{
    const ULONGLONG nowTick = GetTickCount64();
    if (visualSinceTick_ == 0 || visual != lastVisual_) {
        lastVisual_ = visual;
        visualSinceTick_ = nowTick;
        blinkOn_ = true;
    }
}

void TrayApp::ShowContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuOpenCodex, L"打开 Codex");
    AppendMenuW(menu, MF_STRING, kMenuClearCompleted, L"清除完成提示");
    AppendMenuW(menu, MF_STRING, kMenuCopyDiagnostics, L"复制诊断信息");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    POINT cursor {};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void TrayApp::OpenCodex()
{
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"codex", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShellExecuteW(nullptr, L"open", L"Codex.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void TrayApp::ClearCompletedPrompts()
{
    for (const auto& item : snapshot_.sessions) {
        if (item.second.state == TaskState::Completed) {
            acknowledgedCompletedSessions_.insert(item.first);
        }
    }
    UpdateTrayIcon();
}

void TrayApp::CopyDiagnosticsToClipboard()
{
    const std::wstring diagnostics = BuildDiagnostics(AggregateSessions());
    const size_t byteCount = (diagnostics.size() + 1) * sizeof(wchar_t);

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (memory == nullptr) {
        return;
    }

    void* lockedMemory = GlobalLock(memory);
    if (lockedMemory == nullptr) {
        GlobalFree(memory);
        return;
    }

    memcpy(lockedMemory, diagnostics.c_str(), byteCount);
    GlobalUnlock(memory);

    if (!OpenClipboard(hwnd_)) {
        GlobalFree(memory);
        return;
    }

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
}

void TrayApp::HandleTrayMessage(LPARAM lParam)
{
    if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowContextMenu();
        return;
    }

    if (lParam == WM_LBUTTONDBLCLK) {
        const AggregateSnapshot aggregate = AggregateSessions();
        OpenCodex();
        if (aggregate.visual == AggregateVisual::Completed) {
            ClearCompletedPrompts();
        }
    }
}

TrayApp::AggregateSnapshot TrayApp::AggregateSessions() const
{
    AggregateSnapshot aggregate;

    for (const auto& item : snapshot_.sessions) {
        const SessionState& session = item.second;
        switch (session.state) {
        case TaskState::WaitingInput:
            aggregate.waitingCount++;
            break;
        case TaskState::Running:
            aggregate.runningCount++;
            break;
        case TaskState::Completed:
            if (acknowledgedCompletedSessions_.find(item.first) == acknowledgedCompletedSessions_.end()) {
                aggregate.completedCount++;
            }
            break;
        case TaskState::Failed:
            aggregate.failedCount++;
            break;
        case TaskState::Cancelled:
            aggregate.cancelledCount++;
            break;
        case TaskState::Stale:
            aggregate.staleCount++;
            break;
        case TaskState::Unknown:
            aggregate.unknownCount++;
            break;
        }
    }

    if (aggregate.waitingCount > 0) {
        aggregate.visual = AggregateVisual::Waiting;
    } else if (aggregate.runningCount > 0) {
        aggregate.visual = AggregateVisual::Running;
    } else {
        aggregate.visual = AggregateVisual::Completed;
    }

    return aggregate;
}

IconKey TrayApp::BuildIconKey(const AggregateSnapshot& aggregate) const
{
    IconKey key;
    key.warningBadge = snapshot_.hasSourceError || aggregate.staleCount > 0 || aggregate.unknownCount > 0;
    key.blinkOn = true;

    const ULONGLONG nowTick = GetTickCount64();
    const ULONGLONG visualAgeMs = visualSinceTick_ == 0 ? 0 : nowTick - visualSinceTick_;

    switch (aggregate.visual) {
    case AggregateVisual::Waiting:
        key.task = TaskVisual::WaitingInput;
        if (visualAgeMs < kRedFlashDurationMs) {
            key.blinkOn = ((nowTick / kFastFlashIntervalMs) % 2) == 0;
        }
        break;
    case AggregateVisual::Running:
        key.task = TaskVisual::Running;
        {
            const ULONGLONG phaseMs = nowTick % kRunningBreathPeriodMs;
            const ULONGLONG halfPeriodMs = kRunningBreathPeriodMs / 2;
            const ULONGLONG risingMs = phaseMs <= halfPeriodMs ? phaseMs : kRunningBreathPeriodMs - phaseMs;
            key.animationLevel = static_cast<int>(risingMs * 10 / halfPeriodMs);
        }
        break;
    case AggregateVisual::Completed:
    default:
        key.task = TaskVisual::Completed;
        if (visualAgeMs < kGreenFlashDurationMs) {
            key.blinkOn = ((nowTick / kFastFlashIntervalMs) % 2) == 0;
        }
        break;
    }

    const bool quotaIsUnavailable =
        snapshot_.quota.validity == QuotaValidity::Unavailable ||
        snapshot_.quota.validity == QuotaValidity::Stale ||
        snapshot_.quota.validity == QuotaValidity::Ambiguous;

    if (quotaIsUnavailable) {
        key.quota = QuotaVisual::Unavailable;
        key.quotaBucket = -1;
    } else {
        key.quota = snapshot_.quota.validity == QuotaValidity::Valid ? QuotaVisual::Valid : QuotaVisual::Partial;
        key.quotaBucket = static_cast<int>(snapshot_.quota.effectiveRemaining / 5.0) * 5;
        if (key.quotaBucket < 0) {
            key.quotaBucket = 0;
        }
        if (key.quotaBucket > 100) {
            key.quotaBucket = 100;
        }
    }

    return key;
}

std::wstring TrayApp::BuildTooltip(const AggregateSnapshot& aggregate) const
{
    std::wostringstream output;
    const size_t completedLikeCount =
        aggregate.completedCount +
        aggregate.failedCount +
        aggregate.cancelledCount;

    output << L"Codex Status Light\n";
    output << L"当前 " << TaskVisualText(aggregate.visual) << L"\n";
    output << L"等待 " << aggregate.waitingCount
        << L"   运行 " << aggregate.runningCount
        << L"   完成 " << completedLikeCount << L"\n";

    if (snapshot_.quota.validity == QuotaValidity::Unavailable ||
        snapshot_.quota.validity == QuotaValidity::Stale ||
        snapshot_.quota.validity == QuotaValidity::Ambiguous) {
        output << L"额度 未知";
    } else {
        output << L"额度 " << static_cast<int>(snapshot_.quota.effectiveRemaining) << L"%";
    }

    if (snapshot_.hasSourceError) {
        output << L"   数据异常";
    } else if (!watcher_.IsRunning() && !snapshot_.sessionsPath.empty()) {
        output << L"   监听恢复中";
    } else {
        output << L"   监听正常";
    }

    return output.str();
}

std::wstring TrayApp::BuildDiagnostics(const AggregateSnapshot& aggregate) const
{
    std::wostringstream output;
    output << L"StatusLight: 0.3.0\n";
    output << L"Codex root: " << snapshot_.codexRoot << L"\n";
    output << L"Watcher: " << (watcher_.IsRunning() ? L"running" : L"recovering") << L"\n";
    output << L"Tracked files: " << snapshot_.trackedFileCount << L"\n";
    output << L"Sessions: " << snapshot_.sessions.size() << L"\n";
    output << L"Waiting: " << aggregate.waitingCount << L"\n";
    output << L"Running: " << aggregate.runningCount << L"\n";
    output << L"Completed: " << aggregate.completedCount << L"\n";
    output << L"Failed: " << aggregate.failedCount << L"\n";
    output << L"Cancelled: " << aggregate.cancelledCount << L"\n";
    output << L"Stale: " << aggregate.staleCount << L"\n";
    output << L"Codex processes: " << processSnapshot_.codexProcessCount << L"\n";
    output << L"Unknown events: " << snapshot_.unknownEventCount << L"\n";
    output << L"JSON errors: " << snapshot_.parseErrorCount << L"\n";
    output << L"Quota validity: ";

    if (snapshot_.quota.validity == QuotaValidity::Valid) {
        output << L"valid";
    } else if (snapshot_.quota.validity == QuotaValidity::Partial) {
        output << L"partial";
    } else if (snapshot_.quota.validity == QuotaValidity::Stale) {
        output << L"stale";
    } else if (snapshot_.quota.validity == QuotaValidity::Ambiguous) {
        output << L"ambiguous";
    } else {
        output << L"unavailable";
    }
    output << L"\n";

    if (snapshot_.quota.validity == QuotaValidity::Valid ||
        snapshot_.quota.validity == QuotaValidity::Partial) {
        output << L"Quota effective remaining: " << static_cast<int>(snapshot_.quota.effectiveRemaining) << L"%\n";
    }
    if (snapshot_.hasSourceError) {
        output << L"Source error: " << snapshot_.errorMessage << L"\n";
    }

    const std::wstring watcherError = watcher_.LastError();
    if (!watcherError.empty()) {
        output << L"Watcher error: " << watcherError << L"\n";
    }
    if (!processSnapshot_.errorMessage.empty()) {
        output << L"Process monitor error: " << processSnapshot_.errorMessage << L"\n";
    }

    return output.str();
}

std::wstring TrayApp::Widen(const std::string& value) const
{
    if (value.empty()) {
        return std::wstring();
    }
    const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(requiredSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredSize);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring TrayApp::TaskVisualText(AggregateVisual visual) const
{
    switch (visual) {
    case AggregateVisual::Waiting:
        return L"等待";
    case AggregateVisual::Running:
        return L"运行";
    case AggregateVisual::Completed:
    default:
        return L"完成";
    }
}

bool TrayApp::HasUserVisibleTask(const AggregateSnapshot& aggregate) const
{
    return aggregate.waitingCount > 0 ||
        aggregate.runningCount > 0 ||
        aggregate.completedCount > 0 ||
        aggregate.failedCount > 0 ||
        aggregate.staleCount > 0 ||
        aggregate.unknownCount > 0;
}

bool TrayApp::ShouldAutoExit(const AggregateSnapshot& aggregate)
{
    if (snapshot_.hasSourceError) {
        return false;
    }
    if (processSnapshot_.codexProcessCount > 0) {
        noCodexSinceTick_ = 0;
        return false;
    }
    if (HasUserVisibleTask(aggregate)) {
        noCodexSinceTick_ = 0;
        return false;
    }

    const ULONGLONG nowTick = GetTickCount64();
    if (noCodexSinceTick_ == 0) {
        noCodexSinceTick_ = nowTick;
    }

    const ULONGLONG startupAge = nowTick - startedAtTick_;
    const ULONGLONG noCodexAge = nowTick - noCodexSinceTick_;
    if (startupAge < kStartupNoCodexGraceMs) {
        return false;
    }

    return noCodexAge >= kRuntimeNoCodexGraceMs;
}
