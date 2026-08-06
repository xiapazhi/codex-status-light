/**
 * 文件作用：实现完成庆祝控制器
 * 职责范围：
 * 1. P0 阶段播放托盘锚定透明测试圆点
 * 2. 按立即、80ms、160ms 节奏重试托盘图标定位
 * 3. 在播放结束后隐藏 overlay 并停止定时器
 *
 * 不负责：
 * - 后续粒子烟花场景
 * - 持久化用户设置
 *
 * 维护说明：
 * - 定位失败只记录诊断并跳过，不影响托盘绿色状态或现有提示逻辑
 */
#include "CelebrationController.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr UINT_PTR kFireworkTimerId = 0x5346;
constexpr UINT kFireworkFrameIntervalMs = 16;
constexpr UINT kRetryDelaysMs[] = { 80, 160 };
constexpr uint32_t kMaxFireworksPerBatch = 8;
constexpr float kStartOffsetPattern[] = { 0.0f, -0.42f, 0.38f, -0.18f, 0.58f, -0.62f, 0.18f, 0.72f };

std::wstring YesNo(bool value)
{
    return value ? L"yes" : L"no";
}

const wchar_t* AudioProfileText(FireworkAudioProfile profile)
{
    return profile == FireworkAudioProfile::Crackle002 ?
        L"firework_explosion_fizz_002" :
        L"firework_explosion_fizz_005";
}

float LaunchDistancePx(const TrayAnchor& anchor, uint32_t launchHeightPercent)
{
    if (launchHeightPercent >= 500) {
        const bool horizontalLaunch =
            anchor.direction == LaunchDirection::Left ||
            anchor.direction == LaunchDirection::Right;
        const LONG monitorSize = horizontalLaunch ?
            anchor.monitorRect.right - anchor.monitorRect.left :
            anchor.monitorRect.bottom - anchor.monitorRect.top;
        return static_cast<float>(std::max<LONG>(1, monitorSize)) * 0.5f;
    }

    return static_cast<float>(ScalePx(72, anchor.dpi)) *
        static_cast<float>(launchHeightPercent) / 100.0f;
}

Vec2 StartOffset(LaunchDirection direction, UINT dpi, uint32_t index)
{
    const Vec2 lateral = LateralAxis(direction);
    const float unit = static_cast<float>(ScalePx(10, dpi));
    const float multiplier = kStartOffsetPattern[index % _countof(kStartOffsetPattern)];
    return lateral * unit * multiplier;
}

} // namespace

bool CelebrationController::Initialize(HINSTANCE instance, HWND mainWindow, UINT trayIconId)
{
    instance_ = instance;
    mainWindow_ = mainWindow;
    trayIconId_ = trayIconId;
    settings_ = policy_.LoadSettings();
    diagnostics_.enabled = settings_.fireworksEnabled;
    audioPlayer_.Initialize();
    return overlay_.Initialize(instance_, &diagnostics_);
}

void CelebrationController::Shutdown()
{
    StopTimer();
    audioPlayer_.Shutdown();
    overlay_.Shutdown();
    diagnostics_.active = false;
}

void CelebrationController::OnAggregateTransition(const AggregateTransition& transition)
{
    if (transition.newSuccessCount == 0) {
        return;
    }

    const CelebrationDecision decision = policy_.Evaluate(settings_, std::chrono::steady_clock::time_point());
    if (!decision.canPlay) {
        diagnostics_.suppressedCount++;
        diagnostics_.lastSuppressionReason = policy_.ReasonText(decision.reason);
        return;
    }

    const uint32_t playCount = std::min(transition.newSuccessCount, kMaxFireworksPerBatch);
    retryIndex_ = 0;
    if (!TryStartFirework(playCount)) {
        ScheduleRetry(kRetryDelaysMs[retryIndex_ - 1]);
    }
}

bool CelebrationController::UpdatePendingForTransition(
    bool* pendingSuccessfulCompletion,
    const AggregateTransition& transition)
{
    if (pendingSuccessfulCompletion == nullptr) {
        return false;
    }

    if (transition.newSuccessCount > 0) {
        *pendingSuccessfulCompletion = false;
        return true;
    }

    return false;
}

void CelebrationController::PlayTestDot()
{
    retryIndex_ = 0;
    if (!TryStartFirework(1)) {
        ScheduleRetry(kRetryDelaysMs[retryIndex_ - 1]);
    }
}

void CelebrationController::ToggleEnabled()
{
    settings_.fireworksEnabled = !settings_.fireworksEnabled;
    diagnostics_.enabled = settings_.fireworksEnabled;
    policy_.SaveSettings(settings_);
}

bool CelebrationController::IsEnabled() const noexcept
{
    return settings_.fireworksEnabled;
}

uint32_t CelebrationController::LaunchHeightPercent() const noexcept
{
    return settings_.launchHeightPercent;
}

uint32_t CelebrationController::BurstSizePercent() const noexcept
{
    return settings_.burstSizePercent;
}

void CelebrationController::SetLaunchHeightPercent(uint32_t percent)
{
    settings_.launchHeightPercent = percent;
    policy_.SaveSettings(settings_);
}

void CelebrationController::SetBurstSizePercent(uint32_t percent)
{
    settings_.burstSizePercent = percent;
    policy_.SaveSettings(settings_);
}

void CelebrationController::OnTimer()
{
    if (!activeFireworks_.empty() && activePlacement_.has_value()) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<FireworkScene> scenes;

        for (ActiveFirework& firework : activeFireworks_) {
            firework.animator.Tick(now);
            if (firework.animator.ConsumeExplosionStarted()) {
                const FireworkAudioProfile audioProfile = firework.animator.Scene().audioProfile;
                audioPlayer_.PlayExplosion(audioProfile);
                diagnostics_.lastAudioProfile = AudioProfileText(audioProfile);
                diagnostics_.lastAudioError = audioPlayer_.LastError();
            }

            if (firework.animator.IsRunning()) {
                scenes.push_back(firework.animator.Scene());
                const uint32_t particleCount = static_cast<uint32_t>(firework.animator.Scene().particles.size());
                if (particleCount > diagnostics_.lastParticlePeak) {
                    diagnostics_.lastParticlePeak = particleCount;
                }
            } else {
                const uint32_t durationMs = static_cast<uint32_t>(GetTickCount64() - firework.startedTick);
                if (durationMs > diagnostics_.lastAnimationDurationMs) {
                    diagnostics_.lastAnimationDurationMs = durationMs;
                }
            }
        }

        activeFireworks_.erase(
            std::remove_if(activeFireworks_.begin(), activeFireworks_.end(), [](const ActiveFirework& firework) {
                return !firework.animator.IsRunning();
            }),
            activeFireworks_.end());

        if (scenes.empty()) {
            overlay_.Hide();
            diagnostics_.active = false;
            activePlacement_.reset();
            StopTimer();
            return;
        }

        renderer_.Render(scenes);
        overlay_.Present(renderer_.Surface(), activePlacement_->screenPosition);
        diagnostics_.active = true;
        return;
    }

    if (!overlay_.IsVisible() && retryIndex_ > 0) {
        if (TryStartTestDot()) {
            return;
        }
        if (retryIndex_ <= _countof(kRetryDelaysMs)) {
            ScheduleRetry(kRetryDelaysMs[retryIndex_ - 1]);
            return;
        }
        diagnostics_.suppressedCount++;
        diagnostics_.lastSuppressionReason = L"TrayIconRectUnavailable";
        diagnostics_.active = false;
        retryIndex_ = 0;
        StopTimer();
    }
}

bool CelebrationController::IsTimerRunning() const noexcept
{
    return timerRunning_;
}

const FireworkDiagnostics& CelebrationController::Diagnostics() const noexcept
{
    return diagnostics_;
}

std::wstring CelebrationController::BuildDiagnostics() const
{
    std::wostringstream output;
    const CelebrationDecision environment = policy_.Evaluate(settings_, lastPlayedAt_);
    output << L"Celebration enabled: " << YesNo(diagnostics_.enabled) << L"\n";
    output << L"Celebration active: " << YesNo(diagnostics_.active) << L"\n";
    output << L"Celebration cooldown: " << settings_.cooldownMilliseconds << L"ms\n";
    output << L"Firework launch height: " << settings_.launchHeightPercent << L"%\n";
    output << L"Firework burst size: " << settings_.burstSizePercent << L"%\n";
    output << L"Animations allowed: " << YesNo(environment.animationsAllowed) << L"\n";
    output << L"Notification state: " << environment.notificationState << L"\n";
    output << L"Tray anchor available: " << YesNo(diagnostics_.trayAnchorAvailable) << L"\n";
    output << L"Launch direction: " << DirectionText(diagnostics_.lastLaunchDirection) << L"\n";
    output << L"Overlay size: " << diagnostics_.overlayWidth << L"x" << diagnostics_.overlayHeight << L"\n";
    output << L"Overlay DPI: " << diagnostics_.lastDpi << L"\n";
    output << L"Firework play count: " << diagnostics_.playCount << L"\n";
    output << L"Firework suppressed count: " << diagnostics_.suppressedCount << L"\n";
    output << L"Last suppression reason: " << diagnostics_.lastSuppressionReason << L"\n";
    output << L"Last animation duration: " << diagnostics_.lastAnimationDurationMs << L"ms\n";
    output << L"Last particle peak: " << diagnostics_.lastParticlePeak << L"\n";
    output << L"Last palette: " << diagnostics_.lastPalette << L"\n";
    output << L"Last audio profile: " << diagnostics_.lastAudioProfile << L"\n";
    output << L"Last audio error: " << diagnostics_.lastAudioError << L"\n";
    output << L"Last random seed: " << diagnostics_.lastRandomSeed << L"\n";
    output << L"Layered window failures: " << diagnostics_.layeredWindowFailures << L"\n";
    if (!diagnostics_.lastWin32Operation.empty()) {
        output << L"Last layered operation: " << diagnostics_.lastWin32Operation << L"\n";
        output << L"Last Win32 error: " << diagnostics_.lastWin32Error << L"\n";
    }
    return output.str();
}

bool CelebrationController::TryStartTestDot()
{
    return TryStartFirework(1);
}

bool CelebrationController::TryStartFirework(uint32_t playCount)
{
    TrayAnchorLocator locator(mainWindow_, trayIconId_);
    const std::optional<TrayAnchor> anchor = locator.Locate();
    diagnostics_.trayAnchorAvailable = anchor.has_value();
    if (!anchor.has_value()) {
        diagnostics_.lastSuppressionReason = L"TrayIconRectUnavailable";
        retryIndex_++;
        return false;
    }
    const uint32_t clampedPlayCount = std::max<uint32_t>(1, std::min(playCount, kMaxFireworksPerBatch));

    FireworkLayoutSettings layout;
    layout.launchHeightPercent = settings_.launchHeightPercent;
    layout.burstSizePercent = settings_.burstSizePercent;
    const FireworkOverlayPlacement placement = locator.BuildPlacement(anchor.value(), layout);
    diagnostics_.lastLaunchDirection = anchor->direction;
    diagnostics_.lastDpi = anchor->dpi;
    diagnostics_.overlayWidth = placement.width;
    diagnostics_.overlayHeight = placement.height;

    if (activePlacement_.has_value()) {
        const bool canShareOverlay =
            activePlacement_->width == placement.width &&
            activePlacement_->height == placement.height &&
            activePlacement_->screenPosition.x == placement.screenPosition.x &&
            activePlacement_->screenPosition.y == placement.screenPosition.y;
        if (!canShareOverlay) {
            activeFireworks_.clear();
            activePlacement_.reset();
            overlay_.Hide();
        }
    }

    if (!renderer_.Initialize(placement.width, placement.height)) {
        diagnostics_.suppressedCount++;
        diagnostics_.lastSuppressionReason = L"OverlayInitializationFailed";
        diagnostics_.active = false;
        StopTimer();
        return true;
    }

    if (!overlay_.Show(placement)) {
        diagnostics_.suppressedCount++;
        diagnostics_.lastSuppressionReason = L"OverlayInitializationFailed";
        diagnostics_.active = false;
        StopTimer();
        return true;
    }

    std::vector<FireworkScene> scenes;
    for (const ActiveFirework& firework : activeFireworks_) {
        if (firework.animator.IsRunning()) {
            scenes.push_back(firework.animator.Scene());
        }
    }

    for (uint32_t index = 0; index < clampedPlayCount; ++index) {
        FireworkPlayParameters parameters;
        const Vec2 launchPoint {
            static_cast<float>(placement.launchPointLocal.x),
            static_cast<float>(placement.launchPointLocal.y)
        };
        const Vec2 startOffset = StartOffset(placement.direction, placement.dpi, index);
        parameters.launchPointLocal = {
            launchPoint.x + startOffset.x,
            launchPoint.y + startOffset.y
        };
        parameters.direction = placement.direction;
        parameters.dpi = placement.dpi;
        parameters.launchHeightPercent = settings_.launchHeightPercent;
        parameters.burstSizePercent = settings_.burstSizePercent;
        parameters.launchDistancePx = LaunchDistancePx(anchor.value(), settings_.launchHeightPercent);
        parameters.overlayWidth = placement.width;
        parameters.overlayHeight = placement.height;

        ActiveFirework firework;
        firework.animator.Start(parameters);
        firework.startedTick = GetTickCount64();
        scenes.push_back(firework.animator.Scene());
        diagnostics_.lastParticlePeak = std::max(
            diagnostics_.lastParticlePeak,
            static_cast<uint32_t>(firework.animator.Scene().particles.size()));
        diagnostics_.lastRandomSeed = firework.animator.Scene().randomSeed;
        diagnostics_.lastPalette = firework.animator.Scene().paletteName;
        activeFireworks_.push_back(std::move(firework));
    }

    renderer_.Render(scenes);
    overlay_.Present(renderer_.Surface(), placement.screenPosition);

    activePlacement_ = placement;
    diagnostics_.playCount += clampedPlayCount;
    diagnostics_.active = true;
    diagnostics_.lastAnimationDurationMs = 0;
    diagnostics_.lastSuppressionReason = L"None";
    lastPlayedAt_ = std::chrono::steady_clock::now();
    animationStartedTick_ = GetTickCount64();
    retryIndex_ = 0;
    if (!timerRunning_) {
        SetTimer(mainWindow_, kFireworkTimerId, kFireworkFrameIntervalMs, nullptr);
        timerRunning_ = true;
    }
    return true;
}

void CelebrationController::ScheduleRetry(UINT delayMs)
{
    SetTimer(mainWindow_, kFireworkTimerId, delayMs, nullptr);
    timerRunning_ = true;
}

void CelebrationController::StopTimer()
{
    if (!timerRunning_ || mainWindow_ == nullptr) {
        return;
    }
    KillTimer(mainWindow_, kFireworkTimerId);
    timerRunning_ = false;
}

const wchar_t* CelebrationController::DirectionText(LaunchDirection direction) const
{
    switch (direction) {
    case LaunchDirection::Down:
        return L"down";
    case LaunchDirection::Left:
        return L"left";
    case LaunchDirection::Right:
        return L"right";
    case LaunchDirection::Up:
    default:
        return L"up";
    }
}
