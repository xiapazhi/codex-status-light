/**
 * 文件作用：声明完成庆祝控制器
 * 职责范围：
 * 1. 在 P0 阶段协调托盘锚点定位和透明测试窗口
 * 2. 管理测试播放定时器和失败重试
 * 3. 向诊断信息暴露当前庆祝模块状态
 *
 * 不负责：
 * - 当前任务状态聚合
 * - 判断成功、失败、取消的业务语义
 *
 * 维护说明：
 * - 后续 P4 会在这里接入 AggregateTransition，不应让状态判断散落到窗口或渲染层
 */
#pragma once

#include "FireworkDiagnostics.h"
#include "FireworkAudioPlayer.h"
#include "FireworkAnimator.h"
#include "FireworkOverlayWindow.h"
#include "CelebrationPolicy.h"
#include "FireworkRenderer.h"
#include "TrayAnchorLocator.h"

#include <Windows.h>

#include <chrono>
#include <optional>
#include <string>

class CelebrationController {
public:
    bool Initialize(HINSTANCE instance, HWND mainWindow, UINT trayIconId);
    void Shutdown();
    void OnAggregateTransition(const AggregateTransition& transition);
    void PlayTestDot();
    void ToggleEnabled();
    bool IsEnabled() const noexcept;
    uint32_t LaunchHeightPercent() const noexcept;
    uint32_t BurstSizePercent() const noexcept;
    void SetLaunchHeightPercent(uint32_t percent);
    void SetBurstSizePercent(uint32_t percent);
    void OnTimer();
    bool IsTimerRunning() const noexcept;
    const FireworkDiagnostics& Diagnostics() const noexcept;
    std::wstring BuildDiagnostics() const;
    static bool UpdatePendingForTransition(bool* pendingSuccessfulCompletion, const AggregateTransition& transition);

private:
    bool TryStartTestDot();
    void ScheduleRetry(UINT delayMs);
    void StopTimer();
    const wchar_t* DirectionText(LaunchDirection direction) const;

    HINSTANCE instance_ = nullptr;
    HWND mainWindow_ = nullptr;
    UINT trayIconId_ = 0;
    FireworkOverlayWindow overlay_;
    FireworkRenderer renderer_;
    FireworkAnimator animator_;
    FireworkAudioPlayer audioPlayer_;
    CelebrationPolicy policy_;
    CelebrationSettings settings_;
    FireworkDiagnostics diagnostics_;
    std::chrono::steady_clock::time_point lastPlayedAt_;
    std::optional<FireworkOverlayPlacement> activePlacement_;
    ULONGLONG animationStartedTick_ = 0;
    UINT retryIndex_ = 0;
    bool timerRunning_ = false;
    bool pendingSuccessfulCompletion_ = false;
};
