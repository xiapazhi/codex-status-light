/**
 * 文件作用：声明烟花爆炸音效播放器
 * 职责范围：
 * 1. 从程序目录下的 assets/audio 查找烟花 MP3 资源
 * 2. 使用 Windows MCI 异步播放一次爆炸音效
 * 3. 在关闭程序或下一次播放前清理上一次 MCI 设备
 *
 * 不负责：
 * - 解码音频数据
 * - 控制烟花动画节奏
 * - 管理音量或用户音频设备
 *
 * 维护说明：
 * - 这里依赖系统 MCI MP3 支持，失败时静默跳过音效，不能影响烟花视觉播放。
 */
#pragma once

#include "FireworkScene.h"

#include <Windows.h>

#include <string>

class FireworkAudioPlayer {
public:
    bool Initialize();
    void PlayExplosion(FireworkAudioProfile profile);
    void Shutdown();
    std::wstring LastError() const;

private:
    std::wstring AssetPath(FireworkAudioProfile profile) const;
    bool SendMciCommand(const std::wstring& command);

    std::wstring executableDirectory_;
    std::wstring lastError_;
    bool initialized_ = false;
};
