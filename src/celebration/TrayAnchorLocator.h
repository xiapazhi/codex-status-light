/**
 * 文件作用：声明托盘图标锚点定位器
 * 职责范围：
 * 1. 使用现有托盘图标 hWnd + uID 定位 Shell 通知区域图标
 * 2. 读取图标所在显示器、工作区和 DPI
 * 3. 计算 P0 透明窗口的屏幕位置
 *
 * 不负责：
 * - 创建托盘图标
 * - 播放动画
 *
 * 维护说明：
 * - 定位失败时调用方应跳过烟花，不要猜测屏幕右下角
 */
#pragma once

#include "FireworkTypes.h"

#include <optional>

class TrayAnchorLocator {
public:
    TrayAnchorLocator(HWND trayWindow, UINT trayIconId);

    std::optional<TrayAnchor> Locate() const;
    FireworkOverlayPlacement BuildPlacement(const TrayAnchor& anchor, const FireworkLayoutSettings& layout) const;

private:
    std::optional<RECT> GetTrayIconRect() const;
    UINT DpiForMonitor(HMONITOR monitor) const;
    LaunchDirection DirectionForAnchor(const RECT& iconRect, const RECT& monitorRect) const;
    POINT LaunchPointForIcon(const RECT& iconRect) const;
    POINT ClampOverlayPosition(POINT position, int width, int height, const RECT& monitorRect) const;

    HWND trayWindow_ = nullptr;
    UINT trayIconId_ = 0;
};
