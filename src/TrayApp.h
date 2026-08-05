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
#include "web/WebSourceController.h"

#include <Windows.h>
#include <Shellapi.h>

#include <set>
#include <string>

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

    struct AggregateSnapshot {
        AggregateVisual visual = AggregateVisual::Completed;
        size_t waitingCount = 0;
        size_t runningCount = 0;
        size_t completedCount = 0;
        size_t failedCount = 0;
        size_t cancelledCount = 0;
        size_t staleCount = 0;
        size_t unknownCount = 0;
        WebMonitorHealth monitorHealth = WebMonitorHealth::Normal;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateHiddenWindow(HINSTANCE instance);
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void RefreshState();
    void StartOrRecoverWatcher();
    void UpdateTrayIcon();
    void UpdateVisualTiming(AggregateVisual visual);
    void ShowContextMenu();
    void OpenCodex();
    void ClearCompletedPrompts();
    void CopyDiagnosticsToClipboard();
    void HandleTrayMessage(LPARAM lParam);
    AggregateSnapshot AggregateSessions() const;
    IconKey BuildIconKey(const AggregateSnapshot& aggregate) const;
    std::wstring BuildTooltip(const AggregateSnapshot& aggregate) const;
    std::wstring BuildDiagnostics(const AggregateSnapshot& aggregate) const;
    std::wstring Widen(const std::string& value) const;
    std::wstring TaskVisualText(AggregateVisual visual) const;
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
    std::set<std::string> acknowledgedCompletedSessions_;
    ULONGLONG startedAtTick_ = 0;
    ULONGLONG noCodexSinceTick_ = 0;
    ULONGLONG lastCalibrationTick_ = 0;
    ULONGLONG lastWebPollTick_ = 0;
    ULONGLONG visualSinceTick_ = 0;
    AggregateVisual lastVisual_ = AggregateVisual::Completed;
    bool blinkOn_ = true;
};
