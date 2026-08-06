/**
 * 文件作用：声明完成烟花播放策略
 * 职责范围：
 * 1. 读取和保存完成烟花用户开关
 * 2. 判断冷却、减少动画、全屏、演示和勿扰状态
 * 3. 返回可用于诊断的抑制原因
 *
 * 不负责：
 * - 托盘锚点定位
 * - 动画窗口和粒子渲染
 *
 * 维护说明：
 * - 当前项目没有通用设置文件，用户开关沿用 Windows HKCU 持久化方式
 */
#pragma once

#include <Windows.h>
#include <Shellapi.h>

#include <chrono>
#include <cstdint>
#include <string>

constexpr uint32_t kDefaultFireworkLaunchHeightPercent = 250;
constexpr uint32_t kDefaultFireworkBurstSizePercent = 170;

struct CelebrationSettings {
    bool fireworksEnabled = true;
    uint32_t launchHeightPercent = kDefaultFireworkLaunchHeightPercent;
    uint32_t burstSizePercent = kDefaultFireworkBurstSizePercent;
    bool respectReducedMotion = true;
    bool respectNotificationState = true;
    uint32_t cooldownMilliseconds = 5000;
};

enum class CelebrationSuppressionReason {
    None,
    DisabledByUser,
    ReducedMotion,
    QuietTime,
    FullScreen,
    PresentationMode,
    Cooldown,
    TrayIconRectUnavailable,
    NoSuccessfulCompletion,
    OverlayInitializationFailed
};

struct CelebrationDecision {
    bool canPlay = true;
    CelebrationSuppressionReason reason = CelebrationSuppressionReason::None;
    bool animationsAllowed = true;
    std::wstring notificationState = L"not_checked";
};

class CelebrationPolicy {
public:
    CelebrationSettings LoadSettings() const;
    void SaveSettings(const CelebrationSettings& settings) const;
    CelebrationDecision Evaluate(
        const CelebrationSettings& settings,
        std::chrono::steady_clock::time_point lastPlayedAt) const;
    std::wstring ReasonText(CelebrationSuppressionReason reason) const;

private:
    bool AnimationsAllowed() const;
    bool IsForegroundFullScreen() const;
    std::wstring NotificationStateText(QUERY_USER_NOTIFICATION_STATE state) const;
};
