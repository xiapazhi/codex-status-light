/**
 * 文件作用：声明完成烟花 P1 动画器
 * 职责范围：
 * 1. 启动一次点火和升空动画
 * 2. 按真实时间推进火箭和尾焰粒子
 * 3. 在尾焰自然淡出后标记动画完成
 *
 * 不负责：
 * - 绘制像素
 * - 管理 Win32 定时器
 *
 * 维护说明：
 * - 若系统休眠或卡顿导致帧间隔过长，应直接结束动画，不补算长 dt
 */
#pragma once

#include "FireworkScene.h"

#include <chrono>

class FireworkAnimator {
public:
    void Start(const FireworkPlayParameters& parameters);
    void Stop();
    void Tick(std::chrono::steady_clock::time_point now);
    bool IsRunning() const noexcept;
    const FireworkScene& Scene() const noexcept;

private:
    void UpdateIgnition(float dt);
    void UpdateLaunch(float dt);
    void CreateMainBurst();
    void UpdateBurst(float dt);
    void UpdateParticles(float dt);
    void RemoveExpiredParticles();
    void AddIgnitionParticles();
    void AddRocketTrailParticles();
    void AddBurstSparkParticles(const Vec2& origin);
    void AddMeteorSparkParticles(const Vec2& origin);
    void AddFineSparkParticles(const Vec2& origin);
    Particle MakeRocketTrail();
    Particle MakeBurstParticle(const Vec2& origin, ParticleKind kind, float angle, float speed, const ColorF& color);
    bool IsSceneFinished() const;
    FireworkPalette PickPalette();

    FireworkScene scene_;
    bool running_ = false;
    std::chrono::steady_clock::time_point previousTick_;
    Random random_ { 1 };
};
