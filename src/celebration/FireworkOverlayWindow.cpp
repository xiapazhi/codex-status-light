/**
 * 文件作用：实现完成烟花透明覆盖窗口
 * 职责范围：
 * 1. 管理 Win32 layered window 和 32 位顶向下 DIB
 * 2. 用预乘 Alpha 绘制 P0 柔和圆点
 * 3. 提交透明画面并在播放结束后隐藏窗口
 *
 * 不负责：
 * - 粒子动画和状态触发
 * - 读取或保存用户设置
 *
 * 维护说明：
 * - 不使用 GDI Alpha 线条，后续粒子也应走逐像素预乘混合
 */
#include "FireworkOverlayWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr DWORD kOverlayExStyle =
    WS_EX_LAYERED |
    WS_EX_TRANSPARENT |
    WS_EX_NOACTIVATE |
    WS_EX_TOOLWINDOW |
    WS_EX_TOPMOST;

constexpr DWORD kOverlayStyle = WS_POPUP;
const wchar_t* kOverlayClassName = L"CodexStatusLightFireworkOverlay";

BYTE FloatToByte(float value)
{
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    return static_cast<BYTE>(clamped * 255.0f + 0.5f);
}

float SmoothStep(float edge0, float edge1, float x)
{
    if (edge0 == edge1) {
        return x < edge0 ? 0.0f : 1.0f;
    }

    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

FireworkOverlayWindow::FireworkOverlayWindow() = default;

FireworkOverlayWindow::~FireworkOverlayWindow()
{
    Shutdown();
}

bool FireworkOverlayWindow::Initialize(HINSTANCE instance, FireworkDiagnostics* diagnostics)
{
    instance_ = instance;
    diagnostics_ = diagnostics;
    return EnsureWindow(instance_);
}

bool FireworkOverlayWindow::ShowTestDot(const FireworkOverlayPlacement& placement)
{
    if (!EnsureWindow(instance_)) {
        return false;
    }
    if (!ResizeSurface(placement.width, placement.height)) {
        return false;
    }

    Clear();

    const float burstOffset = static_cast<float>(ScalePx(58, placement.dpi));
    Vec2 center {
        static_cast<float>(placement.launchPointLocal.x),
        static_cast<float>(placement.launchPointLocal.y)
    };
    if (placement.direction == LaunchDirection::Down) {
        center.y += burstOffset;
    } else if (placement.direction == LaunchDirection::Left) {
        center.x -= burstOffset;
    } else if (placement.direction == LaunchDirection::Right) {
        center.x += burstOffset;
    } else {
        center.y -= burstOffset;
    }
    DrawSoftCircle(center, static_cast<float>(ScalePx(16, placement.dpi)), { 1.0f, 0.82f, 0.24f, 0.9f });
    DrawSoftCircle(center, static_cast<float>(ScalePx(5, placement.dpi)), { 1.0f, 0.98f, 0.88f, 1.0f });

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        placement.screenPosition.x,
        placement.screenPosition.y,
        placement.width,
        placement.height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    visible_ = true;
    return Present(placement.screenPosition);
}

void FireworkOverlayWindow::Hide()
{
    if (hwnd_ != nullptr) {
        SetWindowPos(
            hwnd_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    visible_ = false;
}

void FireworkOverlayWindow::Shutdown()
{
    Hide();
    ReleaseSurface();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool FireworkOverlayWindow::IsVisible() const noexcept
{
    return visible_;
}

LRESULT CALLBACK FireworkOverlayWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<FireworkOverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool FireworkOverlayWindow::EnsureWindow(HINSTANCE instance)
{
    if (hwnd_ != nullptr) {
        return true;
    }
    if (instance == nullptr) {
        return false;
    }

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = FireworkOverlayWindow::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kOverlayClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(
        kOverlayExStyle,
        kOverlayClassName,
        L"Codex Status Light Firework",
        kOverlayStyle,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd_ == nullptr) {
        RecordWin32Error(L"CreateWindowExW");
        return false;
    }
    return true;
}

bool FireworkOverlayWindow::ResizeSurface(int width, int height)
{
    if (surface_.memoryDc != nullptr &&
        surface_.bitmap != nullptr &&
        surface_.width == width &&
        surface_.height == height) {
        return true;
    }

    ReleaseSurface();

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        RecordWin32Error(L"GetDC");
        return false;
    }

    surface_.memoryDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (surface_.memoryDc == nullptr) {
        RecordWin32Error(L"CreateCompatibleDC");
        return false;
    }

    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    surface_.bitmap = CreateDIBSection(surface_.memoryDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (surface_.bitmap == nullptr || bits == nullptr) {
        RecordWin32Error(L"CreateDIBSection");
        ReleaseSurface();
        return false;
    }

    surface_.oldBitmap = SelectObject(surface_.memoryDc, surface_.bitmap);
    surface_.pixels = static_cast<uint32_t*>(bits);
    surface_.width = width;
    surface_.height = height;
    surface_.stridePixels = width;
    return true;
}

void FireworkOverlayWindow::ReleaseSurface()
{
    if (surface_.memoryDc != nullptr && surface_.oldBitmap != nullptr) {
        SelectObject(surface_.memoryDc, surface_.oldBitmap);
    }
    if (surface_.bitmap != nullptr) {
        DeleteObject(surface_.bitmap);
    }
    if (surface_.memoryDc != nullptr) {
        DeleteDC(surface_.memoryDc);
    }
    surface_ = {};
}

void FireworkOverlayWindow::Clear()
{
    if (surface_.pixels == nullptr) {
        return;
    }
    const size_t pixelCount = static_cast<size_t>(surface_.stridePixels) * static_cast<size_t>(surface_.height);
    std::memset(surface_.pixels, 0, pixelCount * sizeof(uint32_t));
}

void FireworkOverlayWindow::DrawSoftCircle(const Vec2& center, float radius, const ColorF& color)
{
    if (surface_.pixels == nullptr || radius <= 0.0f) {
        return;
    }

    const int minX = static_cast<int>(std::floor(center.x - radius));
    const int maxX = static_cast<int>(std::ceil(center.x + radius));
    const int minY = static_cast<int>(std::floor(center.y - radius));
    const int maxY = static_cast<int>(std::ceil(center.y + radius));
    const float radiusSquared = radius * radius;

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - center.x;
            const float dy = static_cast<float>(y) + 0.5f - center.y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > radiusSquared) {
                continue;
            }

            const float normalizedDistance = std::sqrt(distanceSquared) / radius;
            const float falloff = 1.0f - SmoothStep(0.0f, 1.0f, normalizedDistance);
            ColorF pixelColor = color;
            pixelColor.a *= falloff * falloff;
            BlendPremultipliedPixel(x, y, pixelColor);
        }
    }
}

void FireworkOverlayWindow::BlendPremultipliedPixel(int x, int y, const ColorF& color)
{
    if (x < 0 || y < 0 || x >= surface_.width || y >= surface_.height) {
        return;
    }

    const float sourceAlpha = Clamp01(color.a);
    if (sourceAlpha <= 0.0f) {
        return;
    }

    uint32_t& pixel = surface_.pixels[y * surface_.stridePixels + x];
    const float destinationBlue = static_cast<float>(pixel & 0xff) / 255.0f;
    const float destinationGreen = static_cast<float>((pixel >> 8) & 0xff) / 255.0f;
    const float destinationRed = static_cast<float>((pixel >> 16) & 0xff) / 255.0f;
    const float destinationAlpha = static_cast<float>((pixel >> 24) & 0xff) / 255.0f;

    const float sourceRed = Clamp01(color.r) * sourceAlpha;
    const float sourceGreen = Clamp01(color.g) * sourceAlpha;
    const float sourceBlue = Clamp01(color.b) * sourceAlpha;
    const float inverseSourceAlpha = 1.0f - sourceAlpha;

    const float outputAlpha = sourceAlpha + destinationAlpha * inverseSourceAlpha;
    const float outputRed = sourceRed + destinationRed * inverseSourceAlpha;
    const float outputGreen = sourceGreen + destinationGreen * inverseSourceAlpha;
    const float outputBlue = sourceBlue + destinationBlue * inverseSourceAlpha;

    pixel =
        (static_cast<uint32_t>(FloatToByte(outputAlpha)) << 24) |
        (static_cast<uint32_t>(FloatToByte(outputRed)) << 16) |
        (static_cast<uint32_t>(FloatToByte(outputGreen)) << 8) |
        static_cast<uint32_t>(FloatToByte(outputBlue));
}

bool FireworkOverlayWindow::Present(const POINT& screenPosition)
{
    if (hwnd_ == nullptr || surface_.memoryDc == nullptr) {
        return false;
    }

    POINT destinationPoint = screenPosition;
    POINT sourcePoint { 0, 0 };
    SIZE size { surface_.width, surface_.height };
    BLENDFUNCTION blend {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        RecordWin32Error(L"GetDC");
        return false;
    }

    const BOOL result = UpdateLayeredWindow(
        hwnd_,
        screenDc,
        &destinationPoint,
        &size,
        surface_.memoryDc,
        &sourcePoint,
        0,
        &blend,
        ULW_ALPHA);

    ReleaseDC(nullptr, screenDc);

    if (!result) {
        if (diagnostics_ != nullptr) {
            diagnostics_->layeredWindowFailures++;
        }
        RecordWin32Error(L"UpdateLayeredWindow");
        return false;
    }
    return true;
}

void FireworkOverlayWindow::RecordWin32Error(const wchar_t* operation)
{
    if (diagnostics_ == nullptr) {
        return;
    }
    diagnostics_->lastWin32Operation = operation;
    diagnostics_->lastWin32Error = GetLastError();
}
