/**
 * 文件作用：实现完成烟花 P1 场景数学
 * 职责范围：
 * 1. 提供发射方向坐标轴
 * 2. 更新火箭 ease-out 升空和尾焰粒子运动
 *
 * 不负责：
 * - 粒子生成策略
 * - 具体像素绘制
 *
 * 维护说明：
 * - 运动公式按真实 dt 推进，后续 P2/P3 应继续复用这里的基础函数
 */
#include "FireworkScene.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

Vec2 LaunchAxis(LaunchDirection direction)
{
    switch (direction) {
    case LaunchDirection::Down:
        return { 0.0f, 1.0f };
    case LaunchDirection::Left:
        return { -1.0f, 0.0f };
    case LaunchDirection::Right:
        return { 1.0f, 0.0f };
    case LaunchDirection::Up:
    default:
        return { 0.0f, -1.0f };
    }
}

Vec2 LateralAxis(LaunchDirection direction)
{
    switch (direction) {
    case LaunchDirection::Left:
    case LaunchDirection::Right:
        return { 0.0f, 1.0f };
    case LaunchDirection::Down:
    case LaunchDirection::Up:
    default:
        return { 1.0f, 0.0f };
    }
}

void UpdateRocket(Rocket* rocket, float dt)
{
    rocket->ageSeconds += dt;

    const float t = Clamp01(rocket->ageSeconds / rocket->durationSeconds);
    const float eased = EaseOutCubic(t);

    rocket->previousPosition = rocket->position;
    rocket->position = Lerp(rocket->start, rocket->target, eased);

    const float arc = std::sin(t * kPi) * rocket->horizontalDrift;
    rocket->position += rocket->lateralAxis * arc;
}

void UpdateParticle(Particle* particle, float dt)
{
    particle->ageSeconds += dt;
    particle->previousPosition = particle->position;

    const float damping = std::exp(-particle->dragPerSecond * dt);
    particle->velocity *= damping;
    particle->velocity.y += particle->gravity * dt;
    particle->position += particle->velocity * dt;
}

float NormalizedAge(const Particle& particle)
{
    if (particle.lifetimeSeconds <= 0.0f) {
        return 1.0f;
    }
    return Clamp01(particle.ageSeconds / particle.lifetimeSeconds);
}

