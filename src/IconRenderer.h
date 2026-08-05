/**
 * 文件作用：声明托盘状态图标渲染器
 * 职责范围：
 * 1. 根据任务状态、额度状态和异常角标生成 HICON
 * 2. 缓存已经生成的图标
 * 3. 在程序退出时释放图标资源
 *
 * 不负责：
 * - 解析 Codex JSONL
 * - 操作 Shell_NotifyIconW
 *
 * 维护说明：
 * - 托盘 UI 只能通过 IconKey 请求图标，不应自行绘制状态颜色
 */
#pragma once

#include <Windows.h>

#include <map>

enum class TaskVisual {
    Idle,
    Running,
    WaitingInput,
    Completed,
    Failed,
    Unknown
};

enum class QuotaVisual {
    Valid,
    Partial,
    Unavailable
};

struct IconKey {
    TaskVisual task = TaskVisual::Idle;
    int quotaBucket = -1;
    QuotaVisual quota = QuotaVisual::Unavailable;
    int animationLevel = 0;
    bool warningBadge = false;
    bool blinkOn = false;
    bool appMarker = false;
    bool browserMarker = false;

    bool operator<(const IconKey& other) const;
};

class IconRenderer {
public:
    ~IconRenderer();

    HICON GetIcon(const IconKey& key);

private:
    HICON CreateIcon(const IconKey& key);
    void DrawCircle(unsigned int* pixels, int size, int centerX, int centerY, int radius, unsigned int color);
    void DrawQuotaRing(unsigned int* pixels, int size, const IconKey& key);
    void DrawWarningBadge(unsigned int* pixels, int size);
    void DrawSourceMarkers(unsigned int* pixels, int size, const IconKey& key);
    void DrawSourceArc(unsigned int* pixels, int size, double startAngle, double endAngle);
    unsigned int CenterColor(const IconKey& key) const;
    unsigned int QuotaColor(int quotaBucket) const;
    unsigned int BlendColor(unsigned int lowColor, unsigned int highColor, int level) const;

    std::map<IconKey, HICON> cache_;
};
