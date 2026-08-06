/**
 * 文件作用：定义完成烟花效果的基础数据类型
 * 职责范围：
 * 1. 描述托盘锚点、发射方向和 P0 播放参数
 * 2. 提供 DPI 缩放和轻量数学辅助
 *
 * 不负责：
 * - 创建窗口
 * - 绘制粒子动画
 *
 * 维护说明：
 * - 这里的类型会被后续 P1-P5 复用，避免状态接入和渲染层互相依赖
 */
#pragma once

#include <Windows.h>

#include <algorithm>

enum class LaunchDirection {
    Up,
    Down,
    Left,
    Right
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2 operator+(const Vec2& other) const
    {
        return { x + other.x, y + other.y };
    }

    Vec2 operator-(const Vec2& other) const
    {
        return { x - other.x, y - other.y };
    }

    Vec2 operator*(float scalar) const
    {
        return { x * scalar, y * scalar };
    }

    Vec2& operator+=(const Vec2& other)
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    Vec2& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }
};

struct ColorF {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct TrayAnchor {
    RECT iconRect {};
    RECT monitorRect {};
    RECT workArea {};
    HMONITOR monitor = nullptr;
    UINT dpi = 96;
    POINT launchPoint {};
    LaunchDirection direction = LaunchDirection::Up;
};

struct FireworkOverlayPlacement {
    POINT screenPosition {};
    int width = 0;
    int height = 0;
    UINT dpi = 96;
    LaunchDirection direction = LaunchDirection::Up;
    POINT launchPointLocal {};
};

inline int ScalePx(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

inline float Clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}
