/**
 * 文件作用：声明 P2 Win32 托盘应用
 * 职责范围：
 * 1. 创建隐藏窗口并注册任务栏通知区域图标
 * 2. 定时读取 StatusReader 快照并更新托盘图标
 * 3. 处理双击、右键菜单和诊断信息复制
 *
 * 不负责：
 * - JSONL 事件解析
 * - 图标像素绘制
 *
 * 维护说明：
 * - 托盘应用只消费聚合后的状态，不应直接读取 Codex JSONL 文件
 */
#pragma once

#include "IconRenderer.h"
#include "DirectoryWatcher.h"
#include "ProcessMonitor.h"
#include "StatusReader.h"
#include "celebration/CelebrationController.h"
#include "web/WebSourceController.h"

#include <Windows.h>
#include <Shellapi.h>

#include <map>
#include <set>
#include <string>
#include <vector>

struct TrayOptions {
    std::wstring codexHome;
    size_t maxFiles = 5;
    int recentHours = 24;
    int pollSeconds = 2;
};

class TrayApp {
public:
    int Run(HINSTANCE instance, const TrayOptions& options);

private:
    enum class AggregateVisual {
        Waiting,
        Running,
        Completed
    };

    enum class PromptSource {
        App,
        Browser
    };

    enum class PromptStage {
        Waiting,
        Completed,
        Running
    };

    enum class PromptOutcome {
        None,
        Success,
        Failed,
        Cancelled
    };

    struct PromptItem {
        PromptSource source = PromptSource::App;
        PromptStage stage = PromptStage::Running;
        PromptOutcome outcome = PromptOutcome::None;
        std::string stableId;
        std::string openKey;
        int64_t changedAtMs = 0;
    };

    struct AggregateSnapshot {
        AggregateVisual visual = AggregateVisual::Completed;
        PromptItem currentPrompt;
        bool hasPrompt = false;
        size_t waitingCount = 0;
        size_t runningCount = 0;
        size_t completedCount = 0;
        size_t failedCount = 0;
        size_t cancelledCount = 0;
        size_t staleCount = 0;
        size_t unknownCount = 0;
        WebMonitorHealth monitorHealth = WebMonitorHealth::Normal;
        bool hasAppPrompt = false;
        bool hasBrowserPrompt = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateHiddenWindow(HINSTANCE instance);
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void RefreshState();
    void StartOrRecoverWatcher();
    void UpdateTrayIcon();
    void UpdateCelebration(const AggregateSnapshot& aggregate);
    void UpdateVisualTiming(const AggregateSnapshot& aggregate);
    void ShowContextMenu();
    void OpenCodex();
    bool ActivateCodexWindow();
    bool OpenMostRecentBrowserTarget();
    void ClearCompletedPrompts();
    void CopyDiagnosticsToClipboard();
    void HandleTrayMessage(LPARAM lParam);
    AggregateSnapshot AggregateSessions() const;
    void AppendAppPromptItems(std::vector<PromptItem>* items) const;
    void AppendBrowserPromptItems(std::vector<PromptItem>* items) const;
    bool SelectPromptItem(const std::vector<PromptItem>& items, PromptItem* selected) const;
    void AcknowledgeCompletedPromptBatch(const std::vector<PromptItem>& items) const;
    std::string AppPromptStableId(const SessionState& session) const;
    std::string BrowserPromptStableId(const WebConversationRecord& conversation) const;
    int PromptPriority(PromptStage stage) const;
    CelebrationVisualState CelebrationVisual(const AggregateSnapshot& aggregate) const;
    void CollectSuccessfulCompletionIds(std::set<std::string>* ids) const;
    IconKey BuildIconKey(const AggregateSnapshot& aggregate) const;
    std::wstring BuildTooltip(const AggregateSnapshot& aggregate) const;
    std::wstring BuildDiagnostics(const AggregateSnapshot& aggregate) const;
    std::wstring Widen(const std::string& value) const;
    std::wstring PromptText(const PromptItem& item) const;
    std::wstring TaskVisualText(AggregateVisual visual) const;
    std::string PromptSourceText(PromptSource source) const;
    std::string PromptStageText(PromptStage stage) const;
    void OpenCurrentPrompt(const PromptItem& item);
    bool HasUserVisibleTask(const AggregateSnapshot& aggregate) const;
    bool ShouldAutoExit(const AggregateSnapshot& aggregate);

    TrayOptions options_;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW notifyData_ {};
    StatusReader reader_;
    StatusSnapshot snapshot_;
    DirectoryWatcher watcher_;
    ProcessMonitor processMonitor_;
    ProcessSnapshot processSnapshot_;
    WebSourceController webMonitor_;
    IconRenderer iconRenderer_;
    CelebrationController celebrationController_;
    mutable std::set<std::string> acknowledgedCompletedPromptIds_;
    mutable std::map<std::string, ULONGLONG> completedPromptFirstSeenTick_;
    ULONGLONG startedAtTick_ = 0;
    ULONGLONG noCodexSinceTick_ = 0;
    ULONGLONG lastCalibrationTick_ = 0;
    ULONGLONG lastWebPollTick_ = 0;
    ULONGLONG visualSinceTick_ = 0;
    AggregateVisual lastVisual_ = AggregateVisual::Completed;
    CelebrationVisualState lastCelebrationVisual_ = CelebrationVisualState::Completed;
    std::string lastPromptStableId_;
    std::set<std::string> seenSuccessfulCompletionIds_;
    bool blinkOn_ = true;
    bool celebrationBaselineReady_ = false;
};
