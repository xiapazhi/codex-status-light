/**
 * 文件作用：实现托盘图标锚点定位器
 * 职责范围：
 * 1. 调用 Shell_NotifyIconGetRect 获取真实托盘图标矩形
 * 2. 计算多屏、DPI 和发射方向
 * 3. 为 P0 透明窗口提供稳定定位
 *
 * 不负责：
 * - 失败后的重试节奏
 * - 透明窗口生命周期
 *
 * 维护说明：
 * - 当前项目未使用托盘 GUID，因此必须沿用 hWnd + uID，避免创建第二个通知图标
 */
#include "TrayAnchorLocator.h"

#include <Shellapi.h>

namespace {

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
constexpr int kMdtEffectiveDpi = 0;

LONG RectCenterX(const RECT& rect)
{
    return rect.left + (rect.right - rect.left) / 2;
}

LONG RectCenterY(const RECT& rect)
{
    return rect.top + (rect.bottom - rect.top) / 2;
}

} // namespace

TrayAnchorLocator::TrayAnchorLocator(HWND trayWindow, UINT trayIconId)
    : trayWindow_(trayWindow), trayIconId_(trayIconId)
{
}

std::optional<TrayAnchor> TrayAnchorLocator::Locate() const
{
    const std::optional<RECT> iconRect = GetTrayIconRect();
    if (!iconRect.has_value()) {
        return std::nullopt;
    }

    HMONITOR monitor = MonitorFromRect(&iconRect.value(), MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return std::nullopt;
    }

    MONITORINFO monitorInfo {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return std::nullopt;
    }

    TrayAnchor anchor;
    anchor.iconRect = iconRect.value();
    anchor.monitor = monitor;
    anchor.monitorRect = monitorInfo.rcMonitor;
    anchor.workArea = monitorInfo.rcWork;
    anchor.dpi = DpiForMonitor(monitor);
    anchor.direction = DirectionForAnchor(anchor.iconRect, anchor.monitorRect);
    anchor.launchPoint = LaunchPointForIcon(anchor.iconRect);
    return anchor;
}

FireworkOverlayPlacement TrayAnchorLocator::BuildPlacement(const TrayAnchor& anchor) const
{
    const int overlayWidth = ScalePx(180, anchor.dpi);
    const int overlayHeight = ScalePx(150, anchor.dpi);
    const int iconCenterX = static_cast<int>(RectCenterX(anchor.iconRect));
    const int iconCenterY = static_cast<int>(RectCenterY(anchor.iconRect));
    POINT overlayPosition {};

    if (anchor.direction == LaunchDirection::Down) {
        overlayPosition.x = iconCenterX - overlayWidth / 2;
        overlayPosition.y = anchor.iconRect.bottom - ScalePx(8, anchor.dpi);
    } else if (anchor.direction == LaunchDirection::Left) {
        overlayPosition.x = anchor.iconRect.left - overlayWidth + ScalePx(8, anchor.dpi);
        overlayPosition.y = iconCenterY - overlayHeight / 2;
    } else if (anchor.direction == LaunchDirection::Right) {
        overlayPosition.x = anchor.iconRect.right - ScalePx(8, anchor.dpi);
        overlayPosition.y = iconCenterY - overlayHeight / 2;
    } else {
        overlayPosition.x = iconCenterX - overlayWidth / 2;
        overlayPosition.y = anchor.iconRect.top - overlayHeight + ScalePx(8, anchor.dpi);
    }

    const POINT clampedPosition = ClampOverlayPosition(
        overlayPosition,
        overlayWidth,
        overlayHeight,
        anchor.monitorRect);

    FireworkOverlayPlacement placement;
    placement.width = overlayWidth;
    placement.height = overlayHeight;
    placement.dpi = anchor.dpi;
    placement.direction = anchor.direction;
    placement.screenPosition = clampedPosition;
    placement.launchPointLocal = {
        anchor.launchPoint.x - clampedPosition.x,
        anchor.launchPoint.y - clampedPosition.y
    };
    return placement;
}

std::optional<RECT> TrayAnchorLocator::GetTrayIconRect() const
{
    NOTIFYICONIDENTIFIER identifier {};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = trayWindow_;
    identifier.uID = trayIconId_;

    RECT rect {};
    const HRESULT result = Shell_NotifyIconGetRect(&identifier, &rect);
    if (FAILED(result)) {
        return std::nullopt;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return std::nullopt;
    }
    return rect;
}

UINT TrayAnchorLocator::DpiForMonitor(HMONITOR monitor) const
{
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore != nullptr) {
        auto getDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(shcore, "GetDpiForMonitor"));
        if (getDpiForMonitor != nullptr) {
            UINT dpiX = 96;
            UINT dpiY = 96;
            const HRESULT result = getDpiForMonitor(monitor, kMdtEffectiveDpi, &dpiX, &dpiY);
            FreeLibrary(shcore);
            if (SUCCEEDED(result) && dpiX > 0) {
                return dpiX;
            }
            return 96;
        }
        FreeLibrary(shcore);
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return 96;
    }
    const int dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

LaunchDirection TrayAnchorLocator::DirectionForAnchor(const RECT& iconRect, const RECT& monitorRect) const
{
    const LONG centerX = RectCenterX(iconRect);
    const LONG centerY = RectCenterY(iconRect);

    const LONG left = centerX - monitorRect.left;
    const LONG top = centerY - monitorRect.top;
    const LONG right = monitorRect.right - centerX;
    const LONG bottom = monitorRect.bottom - centerY;

    LONG nearest = bottom;
    LaunchDirection direction = LaunchDirection::Up;
    if (top < nearest) {
        nearest = top;
        direction = LaunchDirection::Down;
    }
    if (left < nearest) {
        nearest = left;
        direction = LaunchDirection::Right;
    }
    if (right < nearest) {
        direction = LaunchDirection::Left;
    }
    return direction;
}

POINT TrayAnchorLocator::LaunchPointForIcon(const RECT& iconRect) const
{
    POINT point {};
    point.x = static_cast<LONG>(RectCenterX(iconRect));
    point.y = static_cast<LONG>(RectCenterY(iconRect));
    return point;
}

POINT TrayAnchorLocator::ClampOverlayPosition(POINT position, int width, int height, const RECT& monitorRect) const
{
    const LONG minX = monitorRect.left;
    const LONG minY = monitorRect.top;
    const LONG maxX = monitorRect.right - width;
    const LONG maxY = monitorRect.bottom - height;

    POINT clamped = position;
    clamped.x = std::max(minX, std::min(clamped.x, maxX));
    clamped.y = std::max(minY, std::min(clamped.y, maxY));
    return clamped;
}
