/**
 * 文件作用：声明完成烟花透明覆盖窗口
 * 职责范围：
 * 1. 创建不激活、鼠标穿透、任务栏不可见的 layered window
 * 2. 显示、移动和隐藏透明覆盖窗口
 * 3. 将 renderer 生成的 DIB 提交到 UpdateLayeredWindow
 *
 * 不负责：
 * - 决定烟花是否允许播放
 * - 粒子系统时间推进
 *
 * 维护说明：
 * - DIB 必须使用预乘 Alpha；释放时先选回旧位图，再删除位图和 DC
 */
#pragma once

#include "FireworkDiagnostics.h"
#include "FireworkTypes.h"

#include <Windows.h>

#include <string>

class FireworkOverlayWindow {
public:
    FireworkOverlayWindow();
    ~FireworkOverlayWindow();

    bool Initialize(HINSTANCE instance, FireworkDiagnostics* diagnostics);
    bool Show(const FireworkOverlayPlacement& placement);
    bool Present(const DibSurface& surface, const POINT& screenPosition);
    void Hide();
    void Shutdown();
    bool IsVisible() const noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow(HINSTANCE instance);
    void RecordWin32Error(const wchar_t* operation);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    FireworkDiagnostics* diagnostics_ = nullptr;
    bool visible_ = false;
};
