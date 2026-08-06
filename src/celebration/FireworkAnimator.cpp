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
#include <cmath>
#include <cstddef>

namespace {

constexpr float kIgnitionSeconds = 0.08f;
constexpr float kLaunchSeconds = 1.40f;
constexpr float kLargeFrameCutoffSeconds = 0.5f;
constexpr float kMaximumFrameSeconds = 0.05f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr size_t kMaxParticles = 128;

uint64_t SeedFromClock()
{
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

float PercentToScale(uint32_t percent)
{
    return std::max(0.75f, std::min(2.6f, static_cast<float>(percent) / 100.0f));
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
    scene_.launchHeightScale = PercentToScale(parameters.launchHeightPercent);
    scene_.burstScale = PercentToScale(parameters.burstSizePercent);
    scene_.randomSeed = seed;
    scene_.palette = PickPalette();
    scene_.audioProfile = PickAudioProfile();
    scene_.particles.reserve(kMaxParticles);
    scene_.secondaryBursts.reserve(4);
    explosionStarted_ = false;

    const Vec2 axis = LaunchAxis(parameters.direction);
    const Vec2 lateral = LateralAxis(parameters.direction);
    const float scaledBurstDistance = static_cast<float>(ScalePx(72, parameters.dpi)) * scene_.launchHeightScale;
    const float burstDistance = parameters.launchDistancePx > 0.0f ? parameters.launchDistancePx : scaledBurstDistance;
    const float driftRange = static_cast<float>(ScalePx(8, parameters.dpi)) * scene_.burstScale;
    const float targetJitterRange = static_cast<float>(ScalePx(10, parameters.dpi)) * scene_.burstScale;
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
    } else if (scene_.stage == FireworkStage::MainBurst || scene_.stage == FireworkStage::Afterglow) {
        UpdateBurst(dt);
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

bool FireworkAnimator::ConsumeExplosionStarted() noexcept
{
    const bool result = explosionStarted_;
    explosionStarted_ = false;
    return result;
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
        CreateMainBurst();
        explosionStarted_ = true;
        scene_.stage = FireworkStage::MainBurst;
    }
}

void FireworkAnimator::CreateMainBurst()
{
    if (scene_.mainBurstCreated) {
        return;
    }

    const Vec2 origin = scene_.rocket.position;
    const float burstScale = scene_.burstScale;
    const bool crackleProfile = scene_.audioProfile == FireworkAudioProfile::Crackle002;
    scene_.flashCore.position = origin;
    scene_.flashCore.color = scene_.palette.core;
    scene_.flashCore.durationSeconds = crackleProfile ? 0.34f : 0.46f;
    scene_.flashCore.startRadius = static_cast<float>(ScalePx(crackleProfile ? 20 : 24, scene_.dpi)) * burstScale;
    scene_.flashCore.active = true;

    scene_.shockwave.position = origin;
    scene_.shockwave.color = scene_.palette.primary;
    scene_.shockwave.durationSeconds = crackleProfile ? 0.74f : 0.98f;
    scene_.shockwave.startRadius = static_cast<float>(ScalePx(crackleProfile ? 7 : 9, scene_.dpi)) * burstScale;
    scene_.shockwave.endRadius = static_cast<float>(ScalePx(crackleProfile ? 48 : 58, scene_.dpi)) * burstScale;
    scene_.shockwave.active = true;

    AddBurstSparkParticles(origin);
    AddMeteorSparkParticles(origin);
    AddFineSparkParticles(origin);
    AddAfterglowParticles(origin);
    AddSecondaryBurstSeeds(origin);
    scene_.mainBurstCreated = true;
}

void FireworkAnimator::UpdateBurst(float dt)
{
    for (SecondaryBurstSeed& seed : scene_.secondaryBursts) {
        if (!seed.triggered) {
            seed.ageSeconds += dt;
        }
    }
    TriggerSecondaryBursts();

    if (scene_.flashCore.active) {
        scene_.flashCore.ageSeconds += dt;
        if (scene_.flashCore.ageSeconds >= scene_.flashCore.durationSeconds) {
            scene_.flashCore.active = false;
        }
    }

    if (scene_.shockwave.active) {
        scene_.shockwave.ageSeconds += dt;
        if (scene_.shockwave.ageSeconds >= scene_.shockwave.durationSeconds) {
            scene_.shockwave.active = false;
        }
    }

    if (!scene_.flashCore.active && !scene_.shockwave.active) {
        scene_.stage = FireworkStage::Afterglow;
    }
}

void FireworkAnimator::TriggerSecondaryBursts()
{
    for (SecondaryBurstSeed& seed : scene_.secondaryBursts) {
        if (seed.triggered || seed.ageSeconds < seed.triggerSeconds) {
            continue;
        }
        AddSecondaryBurstParticles(seed);
        seed.triggered = true;
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
    for (int index = 0; index < count && scene_.particles.size() < kMaxParticles; ++index) {
        scene_.particles.push_back(MakeRocketTrail());
    }
}

void FireworkAnimator::AddBurstSparkParticles(const Vec2& origin)
{
    const int sparkCount = random_.RangeInt(32, 40);
    for (int index = 0; index < sparkCount && scene_.particles.size() < kMaxParticles; ++index) {
        const float baseAngle = kTwoPi * static_cast<float>(index) / static_cast<float>(sparkCount);
        const float angle = baseAngle + random_.Range(-0.08f, 0.08f);
        const float speed = random_.Range(62.0f, 98.0f) * scene_.burstScale;
        const ColorF color = index % 3 == 0 ? scene_.palette.secondary : scene_.palette.primary;
        scene_.particles.push_back(MakeBurstParticle(origin, ParticleKind::BurstSpark, angle, speed, color));
    }
}

void FireworkAnimator::AddMeteorSparkParticles(const Vec2& origin)
{
    const int meteorCount = random_.RangeInt(7, 10);
    for (int index = 0; index < meteorCount && scene_.particles.size() < kMaxParticles; ++index) {
        const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(meteorCount) + random_.Range(-0.18f, 0.18f);
        Particle particle = MakeBurstParticle(origin, ParticleKind::MeteorSpark, angle, random_.Range(104.0f, 140.0f) * scene_.burstScale, scene_.palette.accent);
        particle.drawTrail = true;
        particle.startRadius = random_.Range(2.0f, 2.8f) * scene_.burstScale;
        particle.lifetimeSeconds = random_.Range(1.10f, 1.42f);
        scene_.particles.push_back(particle);
    }
}

void FireworkAnimator::AddFineSparkParticles(const Vec2& origin)
{
    const int fineSparkCount = random_.RangeInt(12, 16);
    for (int index = 0; index < fineSparkCount && scene_.particles.size() < kMaxParticles; ++index) {
        const float angle = random_.Range(0.0f, kTwoPi);
        Particle particle = MakeBurstParticle(origin, ParticleKind::BurstSpark, angle, random_.Range(38.0f, 68.0f) * scene_.burstScale, scene_.palette.secondary);
        particle.startRadius = random_.Range(0.9f, 1.5f) * scene_.burstScale;
        particle.lifetimeSeconds = random_.Range(0.78f, 1.08f);
        scene_.particles.push_back(particle);
    }
}

void FireworkAnimator::AddSecondaryBurstParticles(const SecondaryBurstSeed& seed)
{
    const int sparkCount = random_.RangeInt(8, 12);
    for (int index = 0; index < sparkCount && scene_.particles.size() < kMaxParticles; ++index) {
        const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(sparkCount) + random_.Range(-0.22f, 0.22f);
        Particle particle = MakeBurstParticle(seed.position, ParticleKind::SecondarySpark, angle, random_.Range(30.0f, 62.0f) * scene_.burstScale, seed.color);
        particle.startRadius = random_.Range(0.9f, 1.6f) * scene_.burstScale;
        particle.lifetimeSeconds = random_.Range(0.82f, 1.08f);
        particle.gravity = random_.Range(70.0f, 120.0f);
        scene_.particles.push_back(particle);
    }
}

void FireworkAnimator::AddAfterglowParticles(const Vec2& origin)
{
    const int emberCount = random_.RangeInt(12, 16);
    for (int index = 0; index < emberCount && scene_.particles.size() < kMaxParticles; ++index) {
        const float angle = random_.Range(0.25f, 0.75f) * kTwoPi;
        Particle particle = MakeBurstParticle(origin, ParticleKind::Ember, angle, random_.Range(16.0f, 38.0f) * scene_.burstScale, scene_.palette.accent);
        particle.gravity = random_.Range(110.0f, 160.0f);
        particle.dragPerSecond = random_.Range(1.6f, 2.8f);
        particle.lifetimeSeconds = random_.Range(1.02f, 1.34f);
        particle.startRadius = random_.Range(0.7f, 1.3f) * scene_.burstScale;
        particle.endRadius = 0.05f;
        particle.endColor = { 0.95f, 0.32f, 0.04f, 0.0f };
        scene_.particles.push_back(particle);
    }
}

void FireworkAnimator::AddSecondaryBurstSeeds(const Vec2& origin)
{
    const int seedCount = random_.RangeInt(2, 3);
    for (int index = 0; index < seedCount; ++index) {
        const float angle = random_.Range(0.0f, kTwoPi);
        const float distance = random_.Range(18.0f, 40.0f) * scene_.burstScale;
        SecondaryBurstSeed seed;
        seed.position = origin + Vec2 { std::cos(angle), std::sin(angle) } * distance;
        if (scene_.audioProfile == FireworkAudioProfile::Crackle002) {
            seed.triggerSeconds = random_.Range(0.30f, 0.46f);
        } else {
            seed.triggerSeconds = random_.Range(0.42f, 0.58f);
        }
        seed.color = index % 2 == 0 ? scene_.palette.secondary : scene_.palette.accent;
        scene_.secondaryBursts.push_back(seed);
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
    particle.startRadius = random_.Range(1.5f, 2.5f);
    particle.endRadius = 0.15f;
    particle.startColor = { 1.0f, random_.Range(0.52f, 0.78f), 0.08f, 0.82f };
    particle.endColor = { 1.0f, 0.18f, 0.0f, 0.0f };
    return particle;
}

Particle FireworkAnimator::MakeBurstParticle(const Vec2& origin, ParticleKind kind, float angle, float speed, const ColorF& color)
{
    const Vec2 direction {
        std::cos(angle),
        std::sin(angle)
    };

    Particle particle;
    particle.kind = kind;
    particle.position = origin;
    particle.previousPosition = origin;
    particle.velocity = direction * speed;
    particle.gravity = random_.Range(60.0f, 105.0f);
    particle.dragPerSecond = random_.Range(1.0f, 2.2f);
    particle.lifetimeSeconds = random_.Range(1.08f, 1.46f);
    particle.startRadius = random_.Range(1.3f, 2.2f) * scene_.burstScale;
    particle.endRadius = 0.15f;
    particle.startColor = color;
    particle.endColor = { color.r, color.g * 0.45f, color.b * 0.25f, 0.0f };
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
    const bool secondaryComplete = std::all_of(
        scene_.secondaryBursts.begin(),
        scene_.secondaryBursts.end(),
        [](const SecondaryBurstSeed& seed) {
            return seed.triggered;
        });
    return secondaryComplete && scene_.particles.empty() && !scene_.flashCore.active && !scene_.shockwave.active;
}

FireworkPalette FireworkAnimator::PickPalette()
{
    const int choice = random_.RangeInt(1, 100);
    if (choice <= 50) {
        scene_.paletteName = L"GoldWhite";
        return {
            { 1.00f, 0.98f, 0.90f, 1.0f },
            { 1.00f, 0.72f, 0.18f, 1.0f },
            { 1.00f, 0.90f, 0.52f, 1.0f },
            { 1.00f, 0.45f, 0.08f, 1.0f }
        };
    }
    if (choice <= 80) {
        scene_.paletteName = L"BlueGold";
        return {
            { 1.00f, 1.00f, 1.00f, 1.0f },
            { 0.32f, 0.72f, 1.00f, 1.0f },
            { 0.68f, 0.88f, 1.00f, 1.0f },
            { 1.00f, 0.72f, 0.18f, 1.0f }
        };
    }

    scene_.paletteName = L"PurpleRose";
    return {
        { 1.00f, 0.96f, 1.00f, 1.0f },
        { 0.72f, 0.38f, 1.00f, 1.0f },
        { 1.00f, 0.46f, 0.70f, 1.0f },
        { 0.90f, 0.76f, 1.00f, 1.0f }
    };
}

FireworkAudioProfile FireworkAnimator::PickAudioProfile()
{
    const int choice = random_.RangeInt(1, 100);
    return choice <= 70 ? FireworkAudioProfile::Impact005 : FireworkAudioProfile::Crackle002;
}
