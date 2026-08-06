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

#include "web/WebDebugLog.h"

#include <Shellapi.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSourceChangedMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kRefreshTimer = 1001;
constexpr UINT_PTR kBlinkTimer = 1002;
constexpr UINT_PTR kFireworkTimerId = 0x5346;
constexpr UINT kMenuOpenCodex = 2001;
constexpr UINT kMenuClearCompleted = 2002;
constexpr UINT kMenuCopyDiagnostics = 2003;
constexpr UINT kMenuExit = 2004;
constexpr UINT kMenuToggleWebBridge = 2005;
constexpr UINT kMenuTestFirework = 2006;
constexpr UINT kMenuToggleFireworks = 2007;
constexpr ULONGLONG kCalibrationIntervalMs = 5000;
constexpr ULONGLONG kStartupNoCodexGraceMs = 15000;
constexpr ULONGLONG kRuntimeNoCodexGraceMs = 5000;
constexpr ULONGLONG kWebActivePollIntervalMs = 5000;
constexpr ULONGLONG kWebConnectingPollIntervalMs = 5000;
constexpr ULONGLONG kWebIdlePollIntervalMs = 25000;
constexpr UINT kAnimationTimerMs = 125;
constexpr ULONGLONG kRunningBreathPeriodMs = 2400;
constexpr ULONGLONG kFastFlashIntervalMs = 125;
constexpr ULONGLONG kRedFlashDurationMs = 3000;
constexpr ULONGLONG kGreenFlashDurationMs = 1000;
constexpr ULONGLONG kCompletedPromptVisibleMs = 3000;

struct FindCodexWindowContext {
    HWND hwnd = nullptr;
};

std::wstring ProcessPathForWindow(HWND hwnd);
bool IsCodexProcessWindow(HWND hwnd);

BOOL CALLBACK FindCodexWindowProc(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    wchar_t title[256] {};
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    const std::wstring windowTitle = title;
    if (windowTitle.find(L"Codex") == std::wstring::npos && !IsCodexProcessWindow(hwnd)) {
        return TRUE;
    }

    auto* context = reinterpret_cast<FindCodexWindowContext*>(lParam);
    context->hwnd = hwnd;
    return FALSE;
}

std::wstring ProcessPathForWindow(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return std::wstring();
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return std::wstring();
    }

    wchar_t path[4096] {};
    DWORD pathSize = static_cast<DWORD>(_countof(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &pathSize) != FALSE) {
        result.assign(path, pathSize);
    }
    CloseHandle(process);
    return result;
}

bool IsCodexProcessWindow(HWND hwnd)
{
    const std::wstring processPath = ProcessPathForWindow(hwnd);
    return processPath.find(L"OpenAI.Codex") != std::wstring::npos ||
        processPath.find(L"\\Codex") != std::wstring::npos;
}

} // namespace

int TrayApp::Run(HINSTANCE instance, const TrayOptions& options)
{
    options_ = options;
    startedAtTick_ = GetTickCount64();
    webMonitor_.Enable();
    lastWebPollTick_ = startedAtTick_;

    if (!CreateHiddenWindow(instance)) {
        return 2;
    }

    RefreshState();
    StartOrRecoverWatcher();

    if (!AddTrayIcon()) {
        DestroyWindow(hwnd_);
        return 2;
    }
    celebrationController_.Initialize(instance, hwnd_, kTrayIconId);

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

    if (message == app->taskbarCreatedMessage_) {
        app->AddTrayIcon();
        app->UpdateTrayIcon();
        return 0;
    }

    switch (message) {
    case WM_TIMER:
        if (wParam == kFireworkTimerId) {
            app->celebrationController_.OnTimer();
        } else if (wParam == kRefreshTimer) {
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
        case kMenuTestFirework:
            app->celebrationController_.PlayTestDot();
            return 0;
        case kMenuToggleFireworks:
            app->celebrationController_.ToggleEnabled();
            return 0;
        case kMenuToggleWebBridge:
            if (app->webMonitor_.IsEnabled()) {
                app->webMonitor_.Disable();
                app->lastWebPollTick_ = 0;
            } else {
                app->webMonitor_.Enable();
                app->lastWebPollTick_ = GetTickCount64();
            }
            app->UpdateTrayIcon();
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
        app->celebrationController_.Shutdown();
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
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

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
    notifyData_.uID = kTrayIconId;
    notifyData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notifyData_.uCallbackMessage = kTrayMessage;

    const AggregateSnapshot aggregate = AggregateSessions();
    UpdateVisualTiming(aggregate);
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

    const ULONGLONG nowTick = GetTickCount64();
    const WebAccountState currentWebState = webMonitor_.CurrentState();
    const bool hasActiveWebTask =
        currentWebState.waitingCount > 0 ||
        currentWebState.runningCount > 0;
    ULONGLONG webPollIntervalMs = kWebIdlePollIntervalMs;
    if (hasActiveWebTask) {
        webPollIntervalMs = kWebActivePollIntervalMs;
    } else if (webMonitor_.IsEnabled() && currentWebState.nativeBridgeClients == 0) {
        webPollIntervalMs = kWebConnectingPollIntervalMs;
    }
    const bool shouldPollWeb =
        webMonitor_.IsEnabled() &&
        (lastWebPollTick_ == 0 || nowTick - lastWebPollTick_ >= webPollIntervalMs);
    if (shouldPollWeb) {
        webMonitor_.PollOnce();
        lastWebPollTick_ = nowTick;
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
    UpdateCelebration(aggregate);
    UpdateVisualTiming(aggregate);
    notifyData_.hIcon = iconRenderer_.GetIcon(BuildIconKey(aggregate));

    const std::wstring tooltip = BuildTooltip(aggregate);
    wcsncpy_s(notifyData_.szTip, _countof(notifyData_.szTip), tooltip.c_str(), _TRUNCATE);
    notifyData_.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &notifyData_);
}

void TrayApp::UpdateCelebration(const AggregateSnapshot& aggregate)
{
    std::set<std::string> currentSuccessIds;
    CollectSuccessfulCompletionIds(&currentSuccessIds);

    const CelebrationVisualState currentVisual = CelebrationVisual(aggregate);
    if (!celebrationBaselineReady_) {
        seenSuccessfulCompletionIds_ = currentSuccessIds;
        lastCelebrationVisual_ = currentVisual;
        celebrationBaselineReady_ = true;
        return;
    }

    uint32_t newSuccessCount = 0;
    for (const std::string& id : currentSuccessIds) {
        if (seenSuccessfulCompletionIds_.find(id) == seenSuccessfulCompletionIds_.end()) {
            ++newSuccessCount;
        }
    }
    seenSuccessfulCompletionIds_.insert(currentSuccessIds.begin(), currentSuccessIds.end());

    AggregateTransition transition;
    transition.previousVisual = lastCelebrationVisual_;
    transition.currentVisual = currentVisual;
    transition.newSuccessCount = newSuccessCount;
    transition.waitingCount = static_cast<uint32_t>(aggregate.waitingCount);
    transition.runningCount = static_cast<uint32_t>(aggregate.runningCount);
    transition.completedCount = static_cast<uint32_t>(
        aggregate.completedCount + aggregate.failedCount + aggregate.cancelledCount);
    transition.occurredAt = std::chrono::steady_clock::now();

    lastCelebrationVisual_ = currentVisual;
    celebrationController_.OnAggregateTransition(transition);
}

void TrayApp::UpdateVisualTiming(const AggregateSnapshot& aggregate)
{
    const ULONGLONG nowTick = GetTickCount64();
    const std::string promptStableId = aggregate.hasPrompt ? aggregate.currentPrompt.stableId : "";
    const bool isFirstVisual = visualSinceTick_ == 0;
    const bool visualChanged = aggregate.visual != lastVisual_;
    const bool promptChanged = promptStableId != lastPromptStableId_;
    const bool shouldRestartFlash =
        isFirstVisual ||
        visualChanged ||
        (promptChanged && aggregate.visual != AggregateVisual::Completed);

    lastPromptStableId_ = promptStableId;
    if (shouldRestartFlash) {
        lastVisual_ = aggregate.visual;
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
    AppendMenuW(
        menu,
        MF_STRING,
        kMenuToggleWebBridge,
        webMonitor_.IsEnabled() ? L"停用网页桥接" : L"启用网页桥接");
    HMENU fireworksMenu = CreatePopupMenu();
    if (fireworksMenu != nullptr) {
        AppendMenuW(
            fireworksMenu,
            MF_STRING | (celebrationController_.IsEnabled() ? MF_CHECKED : MF_UNCHECKED),
            kMenuToggleFireworks,
            L"启用");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fireworksMenu), L"完成烟花效果");
    }
    AppendMenuW(menu, MF_STRING, kMenuTestFirework, L"测试完成烟花");
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
    if (ActivateCodexWindow()) {
        return;
    }

    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"codex", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShellExecuteW(nullptr, L"open", L"Codex.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool TrayApp::ActivateCodexWindow()
{
    FindCodexWindowContext context;
    EnumWindows(FindCodexWindowProc, reinterpret_cast<LPARAM>(&context));
    if (context.hwnd == nullptr) {
        return false;
    }

    ShowWindow(context.hwnd, SW_RESTORE);
    BringWindowToTop(context.hwnd);
    SetForegroundWindow(context.hwnd);
    SetWindowPos(context.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(context.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return true;
}

void TrayApp::ClearCompletedPrompts()
{
    std::vector<PromptItem> promptItems;
    AppendAppPromptItems(&promptItems);
    AppendBrowserPromptItems(&promptItems);
    for (const PromptItem& item : promptItems) {
        if (item.stage == PromptStage::Completed) {
            acknowledgedCompletedPromptIds_.insert(item.stableId);
            completedPromptFirstSeenTick_.erase(item.stableId);
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

    if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
        const AggregateSnapshot aggregate = AggregateSessions();
        if (aggregate.hasPrompt) {
            WebDebugLog::WriteUtf8(
                L"Tray",
                "left_click prompt source=" + PromptSourceText(aggregate.currentPrompt.source) +
                " stage=" + PromptStageText(aggregate.currentPrompt.stage) +
                " open_key=" + aggregate.currentPrompt.openKey);
            OpenCurrentPrompt(aggregate.currentPrompt);
        } else {
            if (OpenMostRecentBrowserTarget()) {
                return;
            }
            WebDebugLog::Write(L"Tray", L"left_click no_prompt open_codex");
            OpenCodex();
        }
    }
}

TrayApp::AggregateSnapshot TrayApp::AggregateSessions() const
{
    AggregateSnapshot aggregate;
    std::vector<PromptItem> promptItems;
    AppendAppPromptItems(&promptItems);
    AppendBrowserPromptItems(&promptItems);

    for (int guard = 0; guard < 8; ++guard) {
        PromptItem selected;
        if (!SelectPromptItem(promptItems, &selected)) {
            break;
        }

        if (selected.stage != PromptStage::Completed) {
            aggregate.currentPrompt = selected;
            aggregate.hasPrompt = true;
            break;
        }

        const ULONGLONG nowTick = GetTickCount64();
        const auto firstSeen = completedPromptFirstSeenTick_.find(selected.stableId);
        if (firstSeen == completedPromptFirstSeenTick_.end()) {
            completedPromptFirstSeenTick_[selected.stableId] = nowTick;
            aggregate.currentPrompt = selected;
            aggregate.hasPrompt = true;
            break;
        }

        if (nowTick - firstSeen->second < kCompletedPromptVisibleMs) {
            aggregate.currentPrompt = selected;
            aggregate.hasPrompt = true;
            break;
        }

        AcknowledgeCompletedPromptBatch(promptItems);
        promptItems.clear();
        AppendAppPromptItems(&promptItems);
        AppendBrowserPromptItems(&promptItems);
    }

    for (const PromptItem& item : promptItems) {
        if (acknowledgedCompletedPromptIds_.find(item.stableId) != acknowledgedCompletedPromptIds_.end()) {
            continue;
        }

        if (item.source == PromptSource::App) {
            aggregate.hasAppPrompt = true;
        } else if (item.source == PromptSource::Browser) {
            aggregate.hasBrowserPrompt = true;
        }

        if (item.stage == PromptStage::Waiting) {
            ++aggregate.waitingCount;
        } else if (item.stage == PromptStage::Completed) {
            if (item.outcome == PromptOutcome::Failed) {
                ++aggregate.failedCount;
            } else if (item.outcome == PromptOutcome::Cancelled) {
                ++aggregate.cancelledCount;
            } else {
                ++aggregate.completedCount;
            }
        } else if (item.stage == PromptStage::Running) {
            ++aggregate.runningCount;
        }
    }

    for (const auto& item : snapshot_.sessions) {
        if (item.second.state == TaskState::Stale) {
            aggregate.staleCount++;
        } else if (item.second.state == TaskState::Unknown) {
            aggregate.unknownCount++;
        }
    }

    const WebAccountState webState = webMonitor_.CurrentState();
    aggregate.monitorHealth = webState.health;

    if (aggregate.hasPrompt) {
        if (aggregate.currentPrompt.stage == PromptStage::Waiting) {
            aggregate.visual = AggregateVisual::Waiting;
        } else if (aggregate.currentPrompt.stage == PromptStage::Running) {
            aggregate.visual = AggregateVisual::Running;
        } else {
            aggregate.visual = AggregateVisual::Completed;
        }
    }
    return aggregate;
}

void TrayApp::AppendAppPromptItems(std::vector<PromptItem>* items) const
{
    for (const auto& item : snapshot_.sessions) {
        const SessionState& session = item.second;
        PromptItem prompt;
        prompt.source = PromptSource::App;
        prompt.openKey = session.sessionId;
        prompt.changedAtMs = session.lastEventMs;

        if (session.state == TaskState::WaitingInput) {
            prompt.stage = PromptStage::Waiting;
            prompt.stableId = "A:waiting:" + session.sessionId;
            items->push_back(prompt);
            continue;
        }

        if (session.state == TaskState::Running) {
            prompt.stage = PromptStage::Running;
            prompt.stableId = "A:running:" + session.sessionId;
            items->push_back(prompt);
            continue;
        }

        if (session.state == TaskState::Completed ||
            session.state == TaskState::Failed ||
            session.state == TaskState::Cancelled) {
            prompt.stage = PromptStage::Completed;
            if (session.state == TaskState::Failed) {
                prompt.outcome = PromptOutcome::Failed;
            } else if (session.state == TaskState::Cancelled) {
                prompt.outcome = PromptOutcome::Cancelled;
            } else {
                prompt.outcome = PromptOutcome::Success;
            }
            prompt.stableId = AppPromptStableId(session);
            if (acknowledgedCompletedPromptIds_.find(prompt.stableId) == acknowledgedCompletedPromptIds_.end()) {
                items->push_back(prompt);
            }
        }
    }
}

void TrayApp::AppendBrowserPromptItems(std::vector<PromptItem>* items) const
{
    const WebAccountState webState = webMonitor_.CurrentState();
    for (const WebConversationRecord& conversation : webState.conversations) {
        PromptItem prompt;
        prompt.source = PromptSource::Browser;
        prompt.openKey = conversation.conversationKey;
        prompt.changedAtMs = conversation.stateChangedAt;

        if (conversation.state == WebConversationState::WaitingInput) {
            prompt.stage = PromptStage::Waiting;
            prompt.stableId = "B:waiting:" + conversation.conversationKey;
            items->push_back(prompt);
            continue;
        }

        if (conversation.state == WebConversationState::Running) {
            prompt.stage = PromptStage::Running;
            prompt.stableId = "B:running:" + conversation.conversationKey;
            items->push_back(prompt);
            continue;
        }

        if (conversation.state == WebConversationState::TerminalSuccess ||
            conversation.state == WebConversationState::TerminalFailed ||
            conversation.state == WebConversationState::TerminalCancelled) {
            prompt.stage = PromptStage::Completed;
            if (conversation.state == WebConversationState::TerminalFailed) {
                prompt.outcome = PromptOutcome::Failed;
            } else if (conversation.state == WebConversationState::TerminalCancelled) {
                prompt.outcome = PromptOutcome::Cancelled;
            } else {
                prompt.outcome = PromptOutcome::Success;
            }
            prompt.stableId = BrowserPromptStableId(conversation);
            if (acknowledgedCompletedPromptIds_.find(prompt.stableId) == acknowledgedCompletedPromptIds_.end()) {
                items->push_back(prompt);
            }
        }
    }
}

bool TrayApp::SelectPromptItem(const std::vector<PromptItem>& items, PromptItem* selected) const
{
    bool found = false;
    for (const PromptItem& item : items) {
        if (acknowledgedCompletedPromptIds_.find(item.stableId) != acknowledgedCompletedPromptIds_.end()) {
            continue;
        }

        if (!found ||
            PromptPriority(item.stage) > PromptPriority(selected->stage) ||
            (PromptPriority(item.stage) == PromptPriority(selected->stage) && item.changedAtMs > selected->changedAtMs)) {
            *selected = item;
            found = true;
        }
    }
    return found;
}

void TrayApp::AcknowledgeCompletedPromptBatch(const std::vector<PromptItem>& items) const
{
    for (const PromptItem& item : items) {
        if (item.stage != PromptStage::Completed) {
            continue;
        }

        acknowledgedCompletedPromptIds_.insert(item.stableId);
        completedPromptFirstSeenTick_.erase(item.stableId);
    }
}

std::string TrayApp::AppPromptStableId(const SessionState& session) const
{
    return "A:completed:" + session.sessionId + ":" + session.taskId + ":" + session.lastEventTime;
}

std::string TrayApp::BrowserPromptStableId(const WebConversationRecord& conversation) const
{
    return "B:completed:" + conversation.conversationKey + ":" + std::to_string(conversation.operationGeneration);
}

int TrayApp::PromptPriority(PromptStage stage) const
{
    if (stage == PromptStage::Waiting) {
        return 3;
    }
    if (stage == PromptStage::Completed) {
        return 2;
    }
    return 1;
}

CelebrationVisualState TrayApp::CelebrationVisual(const AggregateSnapshot& aggregate) const
{
    if (aggregate.waitingCount > 0) {
        return CelebrationVisualState::Waiting;
    }
    if (aggregate.runningCount > 0) {
        return CelebrationVisualState::Running;
    }
    return CelebrationVisualState::Completed;
}

void TrayApp::CollectSuccessfulCompletionIds(std::set<std::string>* ids) const
{
    for (const auto& item : snapshot_.sessions) {
        const SessionState& session = item.second;
        if (session.state == TaskState::Completed) {
            ids->insert(AppPromptStableId(session));
        }
    }

    const WebAccountState webState = webMonitor_.CurrentState();
    for (const WebConversationRecord& conversation : webState.conversations) {
        if (conversation.state == WebConversationState::TerminalSuccess) {
            ids->insert(BrowserPromptStableId(conversation));
        }
    }
}

IconKey TrayApp::BuildIconKey(const AggregateSnapshot& aggregate) const
{
    IconKey key;
    key.warningBadge = snapshot_.hasSourceError ||
        aggregate.monitorHealth == WebMonitorHealth::Degraded ||
        aggregate.monitorHealth == WebMonitorHealth::Error ||
        aggregate.staleCount > 0 ||
        aggregate.unknownCount > 0;
    key.blinkOn = true;
    key.appMarker = aggregate.hasAppPrompt;
    key.browserMarker = aggregate.hasBrowserPrompt;

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
    if (aggregate.hasPrompt) {
        output << PromptText(aggregate.currentPrompt) << L"\n";
    } else {
        output << L"当前 " << TaskVisualText(aggregate.visual) << L"\n";
    }
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

    if (snapshot_.hasSourceError || aggregate.monitorHealth == WebMonitorHealth::Error) {
        output << L"   监听异常";
    } else if (aggregate.monitorHealth == WebMonitorHealth::Degraded) {
        output << L"   监听部分异常";
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
    output << webMonitor_.Diagnostics();
    output << celebrationController_.BuildDiagnostics();

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

std::wstring TrayApp::PromptText(const PromptItem& item) const
{
    std::wstring output;
    if (item.stage == PromptStage::Waiting) {
        output += L"等待人工介入";
    } else if (item.stage == PromptStage::Completed) {
        output += L"完成";
    } else {
        output += L"运行中";
    }
    return output;
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

void TrayApp::OpenCurrentPrompt(const PromptItem& item)
{
    if (item.source == PromptSource::Browser) {
        if (webMonitor_.QueueFocusRequest(item.openKey)) {
            WebDebugLog::WriteUtf8(L"Tray", "open browser prompt queued open_key=" + item.openKey);
            return;
        }
        WebDebugLog::WriteUtf8(L"Tray", "open browser prompt fallback_codex open_key=" + item.openKey);
    }
    if (item.source == PromptSource::App) {
        WebDebugLog::WriteUtf8(L"Tray", "open app prompt open_key=" + item.openKey);
    }
    OpenCodex();
}

bool TrayApp::OpenMostRecentBrowserTarget()
{
    const WebAccountState webState = webMonitor_.CurrentState();
    const WebConversationRecord* selected = nullptr;
    for (const WebConversationRecord& conversation : webState.conversations) {
        const bool hasTarget =
            !conversation.activeOwnerBrowserInstanceId.empty() &&
            conversation.activeOwnerTabId != 0;
        if (!hasTarget) {
            continue;
        }

        if (selected == nullptr || conversation.lastObservedAt > selected->lastObservedAt) {
            selected = &conversation;
        }
    }

    if (selected == nullptr) {
        return false;
    }

    if (!webMonitor_.QueueFocusRequest(selected->conversationKey)) {
        WebDebugLog::WriteUtf8(
            L"Tray",
            "left_click no_prompt browser_fallback_failed open_key=" + selected->conversationKey);
        return false;
    }

    WebDebugLog::WriteUtf8(
        L"Tray",
        "left_click no_prompt browser_fallback_queued open_key=" + selected->conversationKey);
    return true;
}

std::string TrayApp::PromptSourceText(PromptSource source) const
{
    return source == PromptSource::Browser ? "browser" : "app";
}

std::string TrayApp::PromptStageText(PromptStage stage) const
{
    if (stage == PromptStage::Waiting) {
        return "waiting";
    }
    if (stage == PromptStage::Completed) {
        return "completed";
    }
    return "running";
}

bool TrayApp::HasUserVisibleTask(const AggregateSnapshot& aggregate) const
{
    return aggregate.waitingCount > 0 ||
        aggregate.runningCount > 0 ||
        aggregate.completedCount > 0 ||
        aggregate.failedCount > 0 ||
        aggregate.cancelledCount > 0 ||
        aggregate.staleCount > 0 ||
        aggregate.unknownCount > 0;
}

bool TrayApp::ShouldAutoExit(const AggregateSnapshot& aggregate)
{
    if (webMonitor_.IsEnabled()) {
        return false;
    }
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
