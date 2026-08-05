/**
 * 文件作用：实现托盘状态图标渲染器
 * 职责范围：
 * 1. 使用 CreateDIBSection 和 CreateIconIndirect 生成 32x32 图标
 * 2. 绘制中心任务状态、外围额度环和异常角标
 * 3. 缓存并释放 HICON 资源
 *
 * 不负责：
 * - 判断当前任务状态
 * - 处理托盘交互
 *
 * 维护说明：
 * - 图标尺寸固定为 32x32，Windows 会按托盘缩放比例自行缩放
 */
#include "IconRenderer.h"

#include <cmath>
#include <vector>

namespace {

constexpr int kIconSize = 32;

unsigned int Argb(unsigned char alpha, unsigned char red, unsigned char green, unsigned char blue)
{
    return (static_cast<unsigned int>(alpha) << 24) |
        (static_cast<unsigned int>(red) << 16) |
        (static_cast<unsigned int>(green) << 8) |
        static_cast<unsigned int>(blue);
}

bool IsPointInCircle(int x, int y, int centerX, int centerY, int radius)
{
    const int dx = x - centerX;
    const int dy = y - centerY;
    return dx * dx + dy * dy <= radius * radius;
}

} // namespace

bool IconKey::operator<(const IconKey& other) const
{
    if (task != other.task) {
        return static_cast<int>(task) < static_cast<int>(other.task);
    }
    if (quotaBucket != other.quotaBucket) {
        return quotaBucket < other.quotaBucket;
    }
    if (quota != other.quota) {
        return static_cast<int>(quota) < static_cast<int>(other.quota);
    }
    if (animationLevel != other.animationLevel) {
        return animationLevel < other.animationLevel;
    }
    if (warningBadge != other.warningBadge) {
        return warningBadge < other.warningBadge;
    }
    return blinkOn < other.blinkOn;
}

IconRenderer::~IconRenderer()
{
    for (const auto& item : cache_) {
        DestroyIcon(item.second);
    }
}

HICON IconRenderer::GetIcon(const IconKey& key)
{
    const auto existing = cache_.find(key);
    if (existing != cache_.end()) {
        return existing->second;
    }

    HICON icon = CreateIcon(key);
    cache_[key] = icon;
    return icon;
}

HICON IconRenderer::CreateIcon(const IconKey& key)
{
    BITMAPV5HEADER header {};
    header.bV5Size = sizeof(header);
    header.bV5Width = kIconSize;
    header.bV5Height = -kIconSize;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC screenDc = GetDC(nullptr);
    void* rawPixels = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (colorBitmap == nullptr || rawPixels == nullptr) {
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    unsigned int* pixels = static_cast<unsigned int*>(rawPixels);
    for (int index = 0; index < kIconSize * kIconSize; ++index) {
        pixels[index] = 0;
    }

    DrawQuotaRing(pixels, kIconSize, key);
    DrawCircle(pixels, kIconSize, 16, 16, 10, CenterColor(key));
    if (key.warningBadge) {
        DrawWarningBadge(pixels, kIconSize);
    }

    HBITMAP maskBitmap = CreateBitmap(kIconSize, kIconSize, 1, 1, nullptr);
    ICONINFO iconInfo {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);

    return icon == nullptr ? LoadIconW(nullptr, IDI_APPLICATION) : icon;
}

void IconRenderer::DrawCircle(unsigned int* pixels, int size, int centerX, int centerY, int radius, unsigned int color)
{
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (IsPointInCircle(x, y, centerX, centerY, radius)) {
                pixels[y * size + x] = color;
            }
        }
    }
}

void IconRenderer::DrawQuotaRing(unsigned int* pixels, int size, const IconKey& key)
{
    const int centerX = 16;
    const int centerY = 16;
    const double outerRadius = 15.0;
    const double innerRadius = 12.0;
    const double quotaAngle = key.quotaBucket <= 0 ? 0.0 : 360.0 * key.quotaBucket / 100.0;
    const unsigned int color = key.quota == QuotaVisual::Unavailable
        ? Argb(255, 120, 124, 132)
        : QuotaColor(key.quotaBucket);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = static_cast<double>(x - centerX);
            const double dy = static_cast<double>(y - centerY);
            const double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < innerRadius || distance > outerRadius) {
                continue;
            }

            double angle = std::atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
            if (angle < 0.0) {
                angle += 360.0;
            }

    if (key.quota == QuotaVisual::Unavailable) {
                const bool dashOn = (static_cast<int>(angle) / 24) % 2 == 0;
                if (dashOn) {
                    pixels[y * size + x] = color;
                }
                continue;
            }

            if (angle <= quotaAngle) {
                pixels[y * size + x] = color;
            } else {
                pixels[y * size + x] = Argb(255, 48, 50, 56);
            }
        }
    }
}

void IconRenderer::DrawWarningBadge(unsigned int* pixels, int size)
{
    DrawCircle(pixels, size, 24, 24, 7, Argb(255, 255, 196, 0));
    for (int y = 19; y <= 24; ++y) {
        pixels[y * size + 24] = Argb(255, 255, 255, 255);
        pixels[y * size + 25] = Argb(255, 255, 255, 255);
    }
    pixels[27 * size + 24] = Argb(255, 255, 255, 255);
    pixels[27 * size + 25] = Argb(255, 255, 255, 255);
}

unsigned int IconRenderer::CenterColor(const IconKey& key) const
{
    switch (key.task) {
    case TaskVisual::Running:
        return BlendColor(
            Argb(255, 205, 122, 0),
            Argb(255, 255, 196, 0),
            key.animationLevel);
    case TaskVisual::WaitingInput:
        return key.blinkOn ? Argb(255, 255, 49, 49) : Argb(255, 150, 19, 27);
    case TaskVisual::Completed:
        return key.blinkOn ? Argb(255, 34, 220, 116) : Argb(255, 21, 125, 72);
    case TaskVisual::Failed:
        return key.blinkOn ? Argb(255, 255, 49, 49) : Argb(255, 150, 19, 27);
    case TaskVisual::Unknown:
    case TaskVisual::Idle:
    default:
        return Argb(255, 105, 109, 117);
    }
}

unsigned int IconRenderer::BlendColor(unsigned int lowColor, unsigned int highColor, int level) const
{
    if (level < 0) {
        level = 0;
    }
    if (level > 10) {
        level = 10;
    }

    const unsigned int lowRed = (lowColor >> 16) & 0xFF;
    const unsigned int lowGreen = (lowColor >> 8) & 0xFF;
    const unsigned int lowBlue = lowColor & 0xFF;
    const unsigned int highRed = (highColor >> 16) & 0xFF;
    const unsigned int highGreen = (highColor >> 8) & 0xFF;
    const unsigned int highBlue = highColor & 0xFF;

    const unsigned int red = lowRed + ((highRed - lowRed) * static_cast<unsigned int>(level) / 10);
    const unsigned int green = lowGreen + ((highGreen - lowGreen) * static_cast<unsigned int>(level) / 10);
    const unsigned int blue = lowBlue + ((highBlue - lowBlue) * static_cast<unsigned int>(level) / 10);
    return Argb(255, static_cast<unsigned char>(red), static_cast<unsigned char>(green), static_cast<unsigned char>(blue));
}

unsigned int IconRenderer::QuotaColor(int quotaBucket) const
{
    if (quotaBucket <= 4) {
        return Argb(255, 255, 49, 49);
    }
    if (quotaBucket <= 19) {
        return Argb(255, 255, 196, 0);
    }
    if (quotaBucket <= 49) {
        return Argb(255, 44, 203, 255);
    }
    return Argb(255, 57, 139, 255);
}
