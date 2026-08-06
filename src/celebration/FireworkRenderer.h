/**
 * 文件作用：声明完成烟花 DIB 渲染器
 * 职责范围：
 * 1. 管理 32 位 BGRA 绘制表面
 * 2. 绘制 P1 火箭、尾焰、柔和光点和拖尾
 * 3. 输出可交给 UpdateLayeredWindow 的预乘 Alpha DIB
 *
 * 不负责：
 * - 创建透明覆盖窗口
 * - 推进动画时间
 *
 * 维护说明：
 * - 所有写入 DIB 的颜色必须是预乘 Alpha，避免透明边缘发黑
 */
#pragma once

#include "FireworkScene.h"
#include "FireworkTypes.h"

class FireworkRenderer {
public:
    FireworkRenderer();
    ~FireworkRenderer();

    bool Initialize(int width, int height);
    bool Resize(int width, int height);
    void Render(const FireworkScene& scene);
    void RenderTestDot(const FireworkOverlayPlacement& placement);
    const DibSurface& Surface() const noexcept;

private:
    void ReleaseSurface();
    void Clear();
    void DrawRocket(const Rocket& rocket);
    void DrawParticle(const Particle& particle);
    void DrawSoftCircle(const Vec2& center, float radius, const ColorF& color);
    void DrawGlowTrail(const Vec2& from, const Vec2& to, float width, const ColorF& color);
    void BlendPremultipliedPixel(int x, int y, const ColorF& color);
    float SmoothStep(float edge0, float edge1, float value) const;

    DibSurface surface_;
};

