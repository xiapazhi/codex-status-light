/**
 * 文件作用：实现完成烟花透明覆盖窗口
 * 职责范围：
 * 1. 管理 Win32 layered window
 * 2. 提交 renderer 生成的预乘 Alpha DIB
 * 3. 在播放结束后隐藏窗口
 *
 * 不负责：
 * - 粒子动画和状态触发
 * - 读取或保存用户设置
 *
 * 维护说明：
 * - 不使用 GDI Alpha 线条，后续粒子也应走逐像素预乘混合
 */
#include "FireworkOverlayWindow.h"

namespace {

constexpr DWORD kOverlayExStyle =
    WS_EX_LAYERED |
    WS_EX_TRANSPARENT |
    WS_EX_NOACTIVATE |
    WS_EX_TOOLWINDOW |
    WS_EX_TOPMOST;

constexpr DWORD kOverlayStyle = WS_POPUP;
const wchar_t* kOverlayClassName = L"CodexStatusLightFireworkOverlay";

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

bool FireworkOverlayWindow::Show(const FireworkOverlayPlacement& placement)
{
    if (!EnsureWindow(instance_)) {
        return false;
    }

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        placement.screenPosition.x,
        placement.screenPosition.y,
        placement.width,
        placement.height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    visible_ = true;
    return true;
}

bool FireworkOverlayWindow::Present(const DibSurface& surface, const POINT& screenPosition)
{
    if (hwnd_ == nullptr || surface.memoryDc == nullptr) {
        return false;
    }

    POINT destinationPoint = screenPosition;
    POINT sourcePoint { 0, 0 };
    SIZE size { surface.width, surface.height };
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
        surface.memoryDc,
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
    case WM_DPICHANGED:
        return 0;
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

void FireworkOverlayWindow::RecordWin32Error(const wchar_t* operation)
{
    if (diagnostics_ == nullptr) {
        return;
    }
    diagnostics_->lastWin32Operation = operation;
    diagnostics_->lastWin32Error = GetLastError();
}
