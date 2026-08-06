/**
 * 文件作用：保存完成烟花效果的诊断快照
 * 职责范围：
 * 1. 记录锚点定位、窗口尺寸、播放次数和失败原因
 * 2. 为“复制诊断信息”提供稳定字段
 *
 * 不负责：
 * - 决定是否播放烟花
 * - 记录高频逐帧日志
 *
 * 维护说明：
 * - 正常跳过烟花不是错误，只记录为 suppression reason
 */
#pragma once

#include "FireworkTypes.h"

#include <cstdint>
#include <string>

struct FireworkDiagnostics {
    bool enabled = true;
    bool active = false;
    bool trayAnchorAvailable = false;
    LaunchDirection lastLaunchDirection = LaunchDirection::Up;
    UINT lastDpi = 96;
    int overlayWidth = 0;
    int overlayHeight = 0;
    uint32_t playCount = 0;
    uint32_t suppressedCount = 0;
    uint32_t layeredWindowFailures = 0;
    std::wstring lastSuppressionReason = L"None";
    std::wstring lastWin32Operation;
    DWORD lastWin32Error = 0;
};

