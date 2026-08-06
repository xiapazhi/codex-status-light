/**
 * 文件作用：实现完成烟花 DIB 渲染器
 * 职责范围：
 * 1. 创建和释放 32 位顶向下 BGRA DIB
 * 2. 使用预乘 Alpha 混合绘制 P1 火箭和尾焰
 * 3. 提供 P0 测试圆点绘制能力
 *
 * 不负责：
 * - 调用 UpdateLayeredWindow
 * - 决定动画是否应该播放
 *
 * 维护说明：
 * - 不依赖 GDI Alpha 线条；拖尾通过沿线段采样软光点实现
 */
#include "FireworkRenderer.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

BYTE FloatToByte(float value)
{
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    return static_cast<BYTE>(clamped * 255.0f + 0.5f);
}

float TrailAlpha(float t)
{
    const float inverse = 1.0f - Clamp01(t);
    return inverse * inverse;
}

float ParticleAlpha(float t)
{
    const float fadeIn = t <= 0.08f ? t / 0.08f : 1.0f;
    const float fadeOutStart = 0.45f;
    float fadeOut = 1.0f;
    if (t > fadeOutStart) {
        fadeOut = 1.0f - Clamp01((t - fadeOutStart) / (1.0f - fadeOutStart));
    }
    return Clamp01(fadeIn) * Clamp01(fadeOut);
}

} // namespace

FireworkRenderer::FireworkRenderer() = default;

FireworkRenderer::~FireworkRenderer()
{
    ReleaseSurface();
}

bool FireworkRenderer::Initialize(int width, int height)
{
    return Resize(width, height);
}

bool FireworkRenderer::Resize(int width, int height)
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
        return false;
    }

    surface_.memoryDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (surface_.memoryDc == nullptr) {
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

void FireworkRenderer::Render(const FireworkScene& scene)
{
    Clear();
    DrawShockwave(scene.shockwave);
    for (const Particle& particle : scene.particles) {
        DrawParticle(particle);
    }
    if (scene.stage == FireworkStage::Launch || scene.stage == FireworkStage::Ignition) {
        DrawRocket(scene.rocket);
    }
    DrawFlashCore(scene.flashCore);
}

void FireworkRenderer::RenderTestDot(const FireworkOverlayPlacement& placement)
{
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
}

const DibSurface& FireworkRenderer::Surface() const noexcept
{
    return surface_;
}

void FireworkRenderer::ReleaseSurface()
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

void FireworkRenderer::Clear()
{
    if (surface_.pixels == nullptr) {
        return;
    }
    const size_t pixelCount = static_cast<size_t>(surface_.stridePixels) * static_cast<size_t>(surface_.height);
    std::memset(surface_.pixels, 0, pixelCount * sizeof(uint32_t));
}

void FireworkRenderer::DrawRocket(const Rocket& rocket)
{
    DrawGlowTrail(rocket.previousPosition, rocket.position, 2.2f, { 1.0f, 0.46f, 0.06f, 0.55f });
    DrawSoftCircle(rocket.position, 5.5f, rocket.glowColor);
    DrawSoftCircle(rocket.position, 2.2f, rocket.coreColor);
}

void FireworkRenderer::DrawParticle(const Particle& particle)
{
    const float age = NormalizedAge(particle);
    const bool isRocketTrail = particle.kind == ParticleKind::RocketTrail;
    const bool isEmber = particle.kind == ParticleKind::Ember;
    const float alpha = isRocketTrail || isEmber ? TrailAlpha(age) : ParticleAlpha(age);
    const float radius = Lerp(particle.startRadius, particle.endRadius, age);
    ColorF color;
    color.r = Lerp(particle.startColor.r, particle.endColor.r, age);
    color.g = Lerp(particle.startColor.g, particle.endColor.g, age);
    color.b = Lerp(particle.startColor.b, particle.endColor.b, age);
    color.a = Lerp(particle.startColor.a, particle.endColor.a, age) * alpha;

    if (particle.drawTrail || isRocketTrail) {
        DrawGlowTrail(particle.previousPosition, particle.position, radius * 0.75f, color);
    }
    DrawSoftCircle(particle.position, radius, color);
}

void FireworkRenderer::DrawFlashCore(const FlashCore& core)
{
    if (!core.active) {
        return;
    }

    const float age = Clamp01(core.ageSeconds / core.durationSeconds);
    const float radius = Lerp(core.startRadius, 2.0f, age);
    ColorF outer = core.color;
    outer.a *= (1.0f - age) * 0.75f;
    DrawSoftCircle(core.position, radius, outer);

    ColorF inner = core.color;
    inner.a *= 1.0f - age;
    DrawSoftCircle(core.position, radius * 0.34f, inner);
}

void FireworkRenderer::DrawShockwave(const Shockwave& shockwave)
{
    if (!shockwave.active) {
        return;
    }

    const float age = Clamp01(shockwave.ageSeconds / shockwave.durationSeconds);
    const float radius = Lerp(shockwave.startRadius, shockwave.endRadius, age);
    ColorF color = shockwave.color;
    color.a *= 1.0f - age;

    const int samples = std::max(18, static_cast<int>(radius * 2.4f));
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (int index = 0; index < samples; ++index) {
        const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(samples);
        const Vec2 point {
            shockwave.position.x + std::cos(angle) * radius,
            shockwave.position.y + std::sin(angle) * radius
        };
        DrawSoftCircle(point, 1.1f, color);
    }
}

void FireworkRenderer::DrawSoftCircle(const Vec2& center, float radius, const ColorF& color)
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

void FireworkRenderer::DrawGlowTrail(const Vec2& from, const Vec2& to, float width, const ColorF& color)
{
    const Vec2 delta = to - from;
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    const int samples = std::max(1, static_cast<int>(std::ceil(length * 1.4f)));

    for (int index = 0; index <= samples; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(samples);
        const Vec2 position = Lerp(from, to, t);
        ColorF sampleColor = color;
        sampleColor.a *= Lerp(0.25f, 1.0f, t);
        DrawSoftCircle(position, width, sampleColor);
    }
}

void FireworkRenderer::BlendPremultipliedPixel(int x, int y, const ColorF& color)
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

float FireworkRenderer::SmoothStep(float edge0, float edge1, float value) const
{
    if (edge0 == edge1) {
        return value < edge0 ? 0.0f : 1.0f;
    }
    const float t = Clamp01((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}
