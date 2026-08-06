/**
 * 文件作用：实现完成烟花 P1 动画器
 * 职责范围：
 * 1. 构造火箭从托盘锚点升空的 P1 场景
 * 2. 生成点火粒子和连续尾焰
 * 3. 使用真实 dt 推进并控制动画结束
 *
 * 不负责：
 * - 主爆炸、二次爆点和余辉
 * - 窗口和 DIB 资源管理
 *
 * 维护说明：
 * - 粒子峰值控制在很小范围内，避免高频定时器期间产生额外压力
 */
#include "FireworkAnimator.h"

#include <algorithm>
#include <chrono>

namespace {

constexpr float kIgnitionSeconds = 0.07f;
constexpr float kLaunchSeconds = 0.32f;
constexpr float kLargeFrameCutoffSeconds = 0.5f;
constexpr float kMaximumFrameSeconds = 0.05f;

uint64_t SeedFromClock()
{
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

} // namespace

void FireworkAnimator::Start(const FireworkPlayParameters& parameters)
{
    const uint64_t seed = parameters.randomSeed == 0 ? SeedFromClock() : parameters.randomSeed;
    random_ = Random(seed);

    scene_ = FireworkScene();
    scene_.stage = FireworkStage::Ignition;
    scene_.dpi = parameters.dpi;
    scene_.direction = parameters.direction;
    scene_.randomSeed = seed;
    scene_.particles.reserve(96);

    const Vec2 axis = LaunchAxis(parameters.direction);
    const Vec2 lateral = LateralAxis(parameters.direction);
    const float burstDistance = static_cast<float>(ScalePx(64, parameters.dpi));
    const float driftRange = static_cast<float>(ScalePx(7, parameters.dpi));
    const float targetJitterRange = static_cast<float>(ScalePx(8, parameters.dpi));
    const float drift = random_.Range(-driftRange, driftRange);

    scene_.rocket.start = parameters.launchPointLocal;
    scene_.rocket.target = parameters.launchPointLocal + axis * burstDistance + lateral * random_.Range(-targetJitterRange, targetJitterRange);
    scene_.rocket.position = scene_.rocket.start;
    scene_.rocket.previousPosition = scene_.rocket.start;
    scene_.rocket.lateralAxis = lateral;
    scene_.rocket.durationSeconds = kLaunchSeconds;
    scene_.rocket.horizontalDrift = drift;

    running_ = true;
    previousTick_ = std::chrono::steady_clock::now();
}

void FireworkAnimator::Stop()
{
    running_ = false;
    scene_.stage = FireworkStage::Finished;
    scene_.particles.clear();
}

void FireworkAnimator::Tick(std::chrono::steady_clock::time_point now)
{
    if (!running_) {
        return;
    }

    float dt = std::chrono::duration<float>(now - previousTick_).count();
    previousTick_ = now;
    if (dt >= kLargeFrameCutoffSeconds) {
        Stop();
        return;
    }
    dt = std::min(dt, kMaximumFrameSeconds);

    scene_.elapsedSeconds += dt;
    if (scene_.stage == FireworkStage::Ignition) {
        UpdateIgnition(dt);
    } else if (scene_.stage == FireworkStage::Launch) {
        UpdateLaunch(dt);
    }

    UpdateParticles(dt);
    RemoveExpiredParticles();

    if (IsSceneFinished()) {
        running_ = false;
        scene_.stage = FireworkStage::Finished;
    }
}

bool FireworkAnimator::IsRunning() const noexcept
{
    return running_;
}

const FireworkScene& FireworkAnimator::Scene() const noexcept
{
    return scene_;
}

void FireworkAnimator::UpdateIgnition(float /*dt*/)
{
    if (!scene_.ignitionCreated) {
        AddIgnitionParticles();
        scene_.ignitionCreated = true;
    }
    if (scene_.elapsedSeconds >= kIgnitionSeconds) {
        scene_.stage = FireworkStage::Launch;
    }
}

void FireworkAnimator::UpdateLaunch(float dt)
{
    UpdateRocket(&scene_.rocket, dt);
    AddRocketTrailParticles();

    const bool launchFinished = scene_.rocket.ageSeconds >= scene_.rocket.durationSeconds;
    if (launchFinished) {
        scene_.rocket.exploded = true;
        scene_.stage = FireworkStage::Afterglow;
    }
}

void FireworkAnimator::UpdateParticles(float dt)
{
    for (Particle& particle : scene_.particles) {
        UpdateParticle(&particle, dt);
    }
}

void FireworkAnimator::RemoveExpiredParticles()
{
    scene_.particles.erase(
        std::remove_if(scene_.particles.begin(), scene_.particles.end(), [](const Particle& particle) {
            return NormalizedAge(particle) >= 1.0f;
        }),
        scene_.particles.end());
}

void FireworkAnimator::AddIgnitionParticles()
{
    const Vec2 axis = LaunchAxis(scene_.direction);
    const Vec2 lateral = LateralAxis(scene_.direction);
    const int count = random_.RangeInt(8, 12);
    for (int index = 0; index < count; ++index) {
        Particle particle;
        particle.kind = ParticleKind::RocketTrail;
        particle.position = scene_.rocket.start + lateral * random_.Range(-4.0f, 4.0f);
        particle.previousPosition = particle.position;
        particle.velocity = axis * random_.Range(10.0f, 22.0f) + lateral * random_.Range(-16.0f, 16.0f);
        particle.gravity = 45.0f;
        particle.dragPerSecond = 4.0f;
        particle.lifetimeSeconds = random_.Range(0.12f, 0.22f);
        particle.startRadius = random_.Range(1.4f, 2.4f);
        particle.endRadius = 0.2f;
        particle.startColor = { 1.0f, 0.86f, 0.28f, 0.85f };
        particle.endColor = { 1.0f, 0.28f, 0.0f, 0.0f };
        scene_.particles.push_back(particle);
    }
}

void FireworkAnimator::AddRocketTrailParticles()
{
    const int count = random_.RangeInt(2, 4);
    for (int index = 0; index < count && scene_.particles.size() < 96; ++index) {
        scene_.particles.push_back(MakeRocketTrail());
    }
}

Particle FireworkAnimator::MakeRocketTrail()
{
    const Vec2 axis = LaunchAxis(scene_.direction);
    const Vec2 lateral = LateralAxis(scene_.direction);

    Particle particle;
    particle.kind = ParticleKind::RocketTrail;
    particle.position = scene_.rocket.position - axis * random_.Range(1.0f, 3.0f);
    particle.previousPosition = particle.position;
    particle.position += lateral * random_.Range(-1.5f, 1.5f);
    particle.velocity = axis * random_.Range(-38.0f, -18.0f) + lateral * random_.Range(-8.0f, 8.0f);
    particle.gravity = 35.0f;
    particle.dragPerSecond = 3.0f;
    particle.lifetimeSeconds = random_.Range(0.08f, 0.17f);
    particle.startRadius = random_.Range(1.3f, 2.2f);
    particle.endRadius = 0.15f;
    particle.startColor = { 1.0f, random_.Range(0.52f, 0.78f), 0.08f, 0.82f };
    particle.endColor = { 1.0f, 0.18f, 0.0f, 0.0f };
    return particle;
}

bool FireworkAnimator::IsSceneFinished() const
{
    if (scene_.stage == FireworkStage::Finished) {
        return true;
    }
    if (scene_.stage != FireworkStage::Afterglow) {
        return false;
    }
    return scene_.particles.empty();
}
