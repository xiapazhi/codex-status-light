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

#include <sstream>

namespace {

constexpr UINT_PTR kFireworkTimerId = 0x5346;
constexpr UINT kFireworkFrameIntervalMs = 16;
constexpr UINT kRetryDelaysMs[] = { 80, 160 };

std::wstring YesNo(bool value)
{
    return value ? L"yes" : L"no";
}

} // namespace

bool CelebrationController::Initialize(HINSTANCE instance, HWND mainWindow, UINT trayIconId)
{
    instance_ = instance;
    mainWindow_ = mainWindow;
    trayIconId_ = trayIconId;
    return overlay_.Initialize(instance_, &diagnostics_);
}

void CelebrationController::Shutdown()
{
    StopTimer();
    overlay_.Shutdown();
    diagnostics_.active = false;
}

void CelebrationController::PlayTestDot()
{
    retryIndex_ = 0;
    if (!TryStartTestDot()) {
        ScheduleRetry(kRetryDelaysMs[retryIndex_ - 1]);
    }
}

void CelebrationController::OnTimer()
{
    if (animator_.IsRunning() && activePlacement_.has_value()) {
        animator_.Tick(std::chrono::steady_clock::now());
        renderer_.Render(animator_.Scene());
        overlay_.Present(renderer_.Surface(), activePlacement_->screenPosition);

        const uint32_t particleCount = static_cast<uint32_t>(animator_.Scene().particles.size());
        if (particleCount > diagnostics_.lastParticlePeak) {
            diagnostics_.lastParticlePeak = particleCount;
        }

        if (!animator_.IsRunning()) {
            overlay_.Hide();
            diagnostics_.active = false;
            diagnostics_.lastAnimationDurationMs = static_cast<uint32_t>(GetTickCount64() - animationStartedTick_);
            activePlacement_.reset();
            StopTimer();
        }
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
    output << L"Celebration enabled: " << YesNo(diagnostics_.enabled) << L"\n";
    output << L"Celebration active: " << YesNo(diagnostics_.active) << L"\n";
    output << L"Celebration cooldown: 0ms\n";
    output << L"Animations allowed: yes\n";
    output << L"Notification state: not_checked_p0\n";
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
    TrayAnchorLocator locator(mainWindow_, trayIconId_);
    const std::optional<TrayAnchor> anchor = locator.Locate();
    diagnostics_.trayAnchorAvailable = anchor.has_value();
    if (!anchor.has_value()) {
        diagnostics_.lastSuppressionReason = L"TrayIconRectUnavailable";
        retryIndex_++;
        return false;
    }

    const FireworkOverlayPlacement placement = locator.BuildPlacement(anchor.value());
    diagnostics_.lastLaunchDirection = anchor->direction;
    diagnostics_.lastDpi = anchor->dpi;
    diagnostics_.overlayWidth = placement.width;
    diagnostics_.overlayHeight = placement.height;

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

    FireworkPlayParameters parameters;
    parameters.launchPointLocal = {
        static_cast<float>(placement.launchPointLocal.x),
        static_cast<float>(placement.launchPointLocal.y)
    };
    parameters.direction = placement.direction;
    parameters.dpi = placement.dpi;
    animator_.Start(parameters);
    renderer_.Render(animator_.Scene());
    overlay_.Present(renderer_.Surface(), placement.screenPosition);

    activePlacement_ = placement;
    diagnostics_.playCount++;
    diagnostics_.active = true;
    diagnostics_.lastParticlePeak = static_cast<uint32_t>(animator_.Scene().particles.size());
    diagnostics_.lastRandomSeed = animator_.Scene().randomSeed;
    diagnostics_.lastAnimationDurationMs = 0;
    diagnostics_.lastPalette = animator_.Scene().paletteName;
    diagnostics_.lastSuppressionReason = L"None";
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
