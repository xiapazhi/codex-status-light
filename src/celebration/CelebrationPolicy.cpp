/**
 * 文件作用：实现完成烟花播放策略
 * 职责范围：
 * 1. 用 HKCU 保存完成烟花开关
 * 2. 检查减少动画、通知状态、全屏和冷却
 * 3. 输出诊断可读的抑制原因
 *
 * 不负责：
 * - 重试被抑制的烟花
 * - 记录高频动画帧信息
 *
 * 维护说明：
 * - 策略判断必须无副作用，播放时间由调用方在真正开始播放后更新
 */
#include "CelebrationPolicy.h"

#include <Shellapi.h>

#include <algorithm>

namespace {

const wchar_t* kSettingsKey = L"Software\\CodexStatusLight";
const wchar_t* kFireworksEnabledValue = L"FireworksEnabled";
const wchar_t* kLaunchHeightPercentValue = L"FireworkLaunchHeightPercent";
const wchar_t* kBurstSizePercentValue = L"FireworkBurstSizePercent";

uint32_t NormalizeLaunchHeightPercent(DWORD value)
{
    if (value == 125 || value == 250 || value == 500) {
        return static_cast<uint32_t>(value);
    }
    if (value == 100) {
        return 125;
    }
    if (value == 220) {
        return 250;
    }
    return kDefaultFireworkLaunchHeightPercent;
}

uint32_t NormalizeBurstSizePercent(DWORD value)
{
    if (value == 100 || value == 170 || value == 240) {
        return static_cast<uint32_t>(value);
    }
    return kDefaultFireworkBurstSizePercent;
}

bool ReadDwordSetting(HKEY key, const wchar_t* name, DWORD* value)
{
    DWORD valueSize = sizeof(*value);
    const LONG readResult = RegQueryValueExW(
        key,
        name,
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(value),
        &valueSize);
    return readResult == ERROR_SUCCESS && valueSize == sizeof(*value);
}

void WriteDwordSetting(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(
        key,
        name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
}

} // namespace

CelebrationSettings CelebrationPolicy::LoadSettings() const
{
    CelebrationSettings settings;

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kSettingsKey,
        0,
        KEY_QUERY_VALUE,
        &key);
    if (openResult != ERROR_SUCCESS) {
        return settings;
    }

    DWORD value = 1;
    const bool hasEnabledValue = ReadDwordSetting(key, kFireworksEnabledValue, &value);
    if (hasEnabledValue) {
        settings.fireworksEnabled = value != 0;
    }

    value = kDefaultFireworkLaunchHeightPercent;
    if (ReadDwordSetting(key, kLaunchHeightPercentValue, &value)) {
        settings.launchHeightPercent = NormalizeLaunchHeightPercent(value);
    }

    value = kDefaultFireworkBurstSizePercent;
    if (ReadDwordSetting(key, kBurstSizePercentValue, &value)) {
        settings.burstSizePercent = NormalizeBurstSizePercent(value);
    }
    RegCloseKey(key);

    return settings;
}

void CelebrationPolicy::SaveSettings(const CelebrationSettings& settings) const
{
    HKEY key = nullptr;
    const LONG createResult = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kSettingsKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (createResult != ERROR_SUCCESS) {
        return;
    }

    const DWORD value = settings.fireworksEnabled ? 1 : 0;
    WriteDwordSetting(key, kFireworksEnabledValue, value);
    WriteDwordSetting(key, kLaunchHeightPercentValue, settings.launchHeightPercent);
    WriteDwordSetting(key, kBurstSizePercentValue, settings.burstSizePercent);
    RegCloseKey(key);
}

CelebrationDecision CelebrationPolicy::Evaluate(
    const CelebrationSettings& settings,
    std::chrono::steady_clock::time_point lastPlayedAt) const
{
    CelebrationDecision decision;
    if (!settings.fireworksEnabled) {
        decision.canPlay = false;
        decision.reason = CelebrationSuppressionReason::DisabledByUser;
        return decision;
    }

    decision.animationsAllowed = AnimationsAllowed();
    if (settings.respectReducedMotion && !decision.animationsAllowed) {
        decision.canPlay = false;
        decision.reason = CelebrationSuppressionReason::ReducedMotion;
        return decision;
    }

    QUERY_USER_NOTIFICATION_STATE notificationState = QUNS_ACCEPTS_NOTIFICATIONS;
    const HRESULT notificationResult = SHQueryUserNotificationState(&notificationState);
    if (SUCCEEDED(notificationResult)) {
        decision.notificationState = NotificationStateText(notificationState);
        if (settings.respectNotificationState) {
            if (notificationState == QUNS_BUSY ||
                notificationState == QUNS_RUNNING_D3D_FULL_SCREEN) {
                decision.canPlay = false;
                decision.reason = CelebrationSuppressionReason::FullScreen;
                return decision;
            }
            if (notificationState == QUNS_PRESENTATION_MODE ||
                notificationState == QUNS_QUIET_TIME) {
                decision.canPlay = false;
                decision.reason = notificationState == QUNS_PRESENTATION_MODE ?
                    CelebrationSuppressionReason::PresentationMode :
                    CelebrationSuppressionReason::QuietTime;
                return decision;
            }
        }
    } else {
        decision.notificationState = L"query_failed";
    }

    if (IsForegroundFullScreen()) {
        decision.canPlay = false;
        decision.reason = CelebrationSuppressionReason::FullScreen;
        return decision;
    }

    if (lastPlayedAt.time_since_epoch().count() > 0) {
        const auto elapsed = std::chrono::steady_clock::now() - lastPlayedAt;
        const auto cooldown = std::chrono::milliseconds(settings.cooldownMilliseconds);
        if (elapsed < cooldown) {
            decision.canPlay = false;
            decision.reason = CelebrationSuppressionReason::Cooldown;
            return decision;
        }
    }

    return decision;
}

std::wstring CelebrationPolicy::ReasonText(CelebrationSuppressionReason reason) const
{
    switch (reason) {
    case CelebrationSuppressionReason::DisabledByUser:
        return L"DisabledByUser";
    case CelebrationSuppressionReason::ReducedMotion:
        return L"ReducedMotion";
    case CelebrationSuppressionReason::QuietTime:
        return L"QuietTime";
    case CelebrationSuppressionReason::FullScreen:
        return L"FullScreen";
    case CelebrationSuppressionReason::PresentationMode:
        return L"PresentationMode";
    case CelebrationSuppressionReason::Cooldown:
        return L"Cooldown";
    case CelebrationSuppressionReason::TrayIconRectUnavailable:
        return L"TrayIconRectUnavailable";
    case CelebrationSuppressionReason::NoSuccessfulCompletion:
        return L"NoSuccessfulCompletion";
    case CelebrationSuppressionReason::OverlayInitializationFailed:
        return L"OverlayInitializationFailed";
    case CelebrationSuppressionReason::None:
    default:
        return L"None";
    }
}

bool CelebrationPolicy::AnimationsAllowed() const
{
    BOOL animationsEnabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsEnabled, 0)) {
        return true;
    }
    return animationsEnabled != FALSE;
}

bool CelebrationPolicy::IsForegroundFullScreen() const
{
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }

    RECT windowRect {};
    if (!GetWindowRect(foreground, &windowRect)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return false;
    }

    MONITORINFO monitorInfo {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    constexpr LONG tolerance = 2;
    return windowRect.left <= monitorInfo.rcMonitor.left + tolerance &&
        windowRect.top <= monitorInfo.rcMonitor.top + tolerance &&
        windowRect.right >= monitorInfo.rcMonitor.right - tolerance &&
        windowRect.bottom >= monitorInfo.rcMonitor.bottom - tolerance;
}

std::wstring CelebrationPolicy::NotificationStateText(QUERY_USER_NOTIFICATION_STATE state) const
{
    switch (state) {
    case QUNS_NOT_PRESENT:
        return L"not_present";
    case QUNS_BUSY:
        return L"busy";
    case QUNS_RUNNING_D3D_FULL_SCREEN:
        return L"running_d3d_full_screen";
    case QUNS_PRESENTATION_MODE:
        return L"presentation_mode";
    case QUNS_ACCEPTS_NOTIFICATIONS:
        return L"accepts_notifications";
    case QUNS_QUIET_TIME:
        return L"quiet_time";
    case QUNS_APP:
        return L"app";
    default:
        return L"unknown";
    }
}
