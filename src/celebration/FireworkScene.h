/**
 * 文件作用：声明完成烟花 P1 场景模型
 * 职责范围：
 * 1. 描述火箭、尾焰粒子和当前动画阶段
 * 2. 保存一次播放所需的坐标轴、DPI 和随机种子
 *
 * 不负责：
 * - 创建 Win32 窗口
 * - 将粒子绘制到 DIB
 *
 * 维护说明：
 * - P1 只包含点火、升空和尾焰；主爆炸和余辉会在 P2/P3 扩展同一场景
 */
#pragma once

#include "FireworkTypes.h"

#include <string>
#include <vector>

enum class FireworkStage {
    Idle,
    Ignition,
    Launch,
    MainBurst,
    Afterglow,
    Finished
};

enum class ParticleKind {
    RocketTrail,
    BurstSpark,
    MeteorSpark,
    SecondarySpark,
    Ember
};

struct Rocket {
    Vec2 start;
    Vec2 target;
    Vec2 position;
    Vec2 previousPosition;
    Vec2 lateralAxis { 1.0f, 0.0f };
    float ageSeconds = 0.0f;
    float durationSeconds = 0.32f;
    float horizontalDrift = 0.0f;
    ColorF coreColor { 1.0f, 0.98f, 0.86f, 1.0f };
    ColorF glowColor { 1.0f, 0.55f, 0.08f, 0.85f };
    bool exploded = false;
};

struct Particle {
    ParticleKind kind = ParticleKind::RocketTrail;
    Vec2 position;
    Vec2 previousPosition;
    Vec2 velocity;
    float gravity = 0.0f;
    float dragPerSecond = 0.0f;
    float ageSeconds = 0.0f;
    float lifetimeSeconds = 0.5f;
    float startRadius = 1.5f;
    float endRadius = 0.2f;
    ColorF startColor;
    ColorF endColor;
    bool drawTrail = false;
};

struct FireworkPalette {
    ColorF core;
    ColorF primary;
    ColorF secondary;
    ColorF accent;
};

struct FlashCore {
    Vec2 position;
    float ageSeconds = 0.0f;
    float durationSeconds = 0.18f;
    float startRadius = 18.0f;
    ColorF color { 1.0f, 0.98f, 0.9f, 1.0f };
    bool active = false;
};

struct Shockwave {
    Vec2 position;
    float ageSeconds = 0.0f;
    float durationSeconds = 0.32f;
    float startRadius = 6.0f;
    float endRadius = 42.0f;
    ColorF color { 1.0f, 0.72f, 0.18f, 0.55f };
    bool active = false;
};

struct FireworkPlayParameters {
    Vec2 launchPointLocal;
    Vec2 burstPointLocal;
    LaunchDirection direction = LaunchDirection::Up;
    UINT dpi = 96;
    uint64_t randomSeed = 0;
};

struct FireworkScene {
    FireworkStage stage = FireworkStage::Idle;
    float elapsedSeconds = 0.0f;
    Rocket rocket;
    FlashCore flashCore;
    Shockwave shockwave;
    std::vector<Particle> particles;
    FireworkPalette palette;
    std::wstring paletteName = L"GoldWhite";
    UINT dpi = 96;
    LaunchDirection direction = LaunchDirection::Up;
    uint64_t randomSeed = 0;
    bool ignitionCreated = false;
    bool mainBurstCreated = false;
};

Vec2 LaunchAxis(LaunchDirection direction);
Vec2 LateralAxis(LaunchDirection direction);
void UpdateRocket(Rocket* rocket, float dt);
void UpdateParticle(Particle* particle, float dt);
float NormalizedAge(const Particle& particle);
