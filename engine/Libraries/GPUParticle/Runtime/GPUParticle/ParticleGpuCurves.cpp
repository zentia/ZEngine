#include "ParticleGpuCurves.h"

#include <cstddef>

namespace
{
    // 找到 t 落在哪两个键之间。keys 按 time 升序，t 已夹紧到 [0,1]。
    // 线性扫而不是二分：烘焙期重采样后的键数很少（典型 8~32），扫的比二分快，
    // 也没有二分在重复时间点上的边界分歧（C++ 与 C# 的二分写法很容易写歪）。
    void LocateKeys(const float* times, uint32_t count, float t, uint32_t& i0, uint32_t& i1, float& frac)
    {
        if (count == 0)
        {
            i0 = 0;
            i1 = 0;
            frac = 0.0f;
            return;
        }
        uint32_t hi = count - 1u;
        if (t >= times[hi])
        {
            i0 = hi;
            i1 = hi;
            frac = 0.0f;
            return;
        }
        uint32_t i = 0u;
        while (i + 1u < count && times[i + 1u] <= t)
        {
            ++i;
        }
        i0 = i;
        i1 = i + 1u;
        const float span = times[i1] - times[i0];
        frac = (span > 0.0f) ? (t - times[i0]) / span : 0.0f;
    }
}  // namespace

float ParticleGpuEvaluateCurve(const ParticleGpuCurveKey* keys, uint32_t keyCount, float t)
{
    if (keys == nullptr || keyCount == 0u)
    {
        return 1.0f;  // 空曲线当常量 1：乘法中性，不会把粒子变成 0 尺寸 / 全透明
    }
    // 关键帧时间存在结构体数组里（AoS），不能当成 float* 直接扫，所以单独取一次。
    // 求值只在烘焙 / CPU 镜像里跑，不是每帧热路径，这点拷贝无所谓。
    static constexpr uint32_t kMaxKeys = 256u;
    float times[kMaxKeys];
    if (keyCount > kMaxKeys)
    {
        keyCount = kMaxKeys;
    }
    for (uint32_t i = 0u; i < keyCount; ++i)
    {
        times[i] = keys[i].time;
    }

    float clamped = t;
    if (clamped < 0.0f)
    {
        clamped = 0.0f;
    }
    else if (clamped > 1.0f)
    {
        clamped = 1.0f;
    }

    uint32_t i0 = 0u;
    uint32_t i1 = 0u;
    float frac = 0.0f;
    LocateKeys(times, keyCount, clamped, i0, i1, frac);
    if (i0 == i1)
    {
        return keys[i0].value;
    }
    const float a = keys[i0].value;
    const float b = keys[i1].value;
    return a + (b - a) * frac;
}

float ParticleGpuEvaluateCurveRange(const ParticleGpuCurveRange& range,
                                    const ParticleGpuCurveKey* all_keys,
                                    float t,
                                    float random01)
{
    switch (static_cast<ParticleGpuCurveMode>(range.mode))
    {
        case ParticleGpuCurveMode::Constant:
            return range.constantMin;

        case ParticleGpuCurveMode::Curve:
            return ParticleGpuEvaluateCurve(all_keys + range.keyOffset, range.keyCount, t);

        case ParticleGpuCurveMode::RandomBetweenTwoConstants:
            return range.constantMin + (range.constantMax - range.constantMin) * random01;

        case ParticleGpuCurveMode::RandomBetweenTwoCurves: {
            // 两条曲线的键是紧挨着的两段：min 在 [keyOffset, keyOffset+keyCount)，
            // max 紧接着放同样多条。这样 C# 侧不需要额外的索引表。
            const float lo = ParticleGpuEvaluateCurve(all_keys + range.keyOffset, range.keyCount, t);
            const float hi =
                ParticleGpuEvaluateCurve(all_keys + range.keyOffset + range.keyCount, range.keyCount, t);
            return lo + (hi - lo) * random01;
        }

        default:
            return 1.0f;
    }
}

void ParticleGpuEvaluateGradient(const ParticleGpuGradientRange& range,
                                 const ParticleGpuGradientColorKey* color_keys,
                                 const ParticleGpuGradientAlphaKey* alpha_keys,
                                 float t,
                                 float out_rgba[4])
{
    float clamped = t;
    if (clamped < 0.0f)
    {
        clamped = 0.0f;
    }
    else if (clamped > 1.0f)
    {
        clamped = 1.0f;
    }

    // 颜色
    if (range.colorKeyCount == 0u)
    {
        out_rgba[0] = 1.0f;
        out_rgba[1] = 1.0f;
        out_rgba[2] = 1.0f;
    }
    else
    {
        uint32_t i = 0u;
        while (i + 1u < range.colorKeyCount && color_keys[range.colorKeyOffset + i + 1u].time <= clamped)
        {
            ++i;
        }
        const uint32_t j = (i + 1u < range.colorKeyCount) ? (i + 1u) : i;
        const ParticleGpuGradientColorKey& a = color_keys[range.colorKeyOffset + i];
        const ParticleGpuGradientColorKey& b = color_keys[range.colorKeyOffset + j];
        const float span = b.time - a.time;
        const float frac = (span > 0.0f) ? (clamped - a.time) / span : 0.0f;
        out_rgba[0] = a.r + (b.r - a.r) * frac;
        out_rgba[1] = a.g + (b.g - a.g) * frac;
        out_rgba[2] = a.b + (b.b - a.b) * frac;
    }

    // alpha（独立插值，与 Unity 一致：颜色键和 alpha 键数量可以不同）
    if (range.alphaKeyCount == 0u)
    {
        out_rgba[3] = 1.0f;
    }
    else
    {
        uint32_t i = 0u;
        while (i + 1u < range.alphaKeyCount && alpha_keys[range.alphaKeyOffset + i + 1u].time <= clamped)
        {
            ++i;
        }
        const uint32_t j = (i + 1u < range.alphaKeyCount) ? (i + 1u) : i;
        const ParticleGpuGradientAlphaKey& a = alpha_keys[range.alphaKeyOffset + i];
        const ParticleGpuGradientAlphaKey& b = alpha_keys[range.alphaKeyOffset + j];
        const float span = b.time - a.time;
        const float frac = (span > 0.0f) ? (clamped - a.time) / span : 0.0f;
        out_rgba[3] = a.a + (b.a - a.a) * frac;
    }
}

uint32_t ParticleGpuHashU32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float ParticleGpuRandom01(uint32_t& state)
{
    state = ParticleGpuHashU32(state);
    // 取高 24 位：低位在乘法哈希里质量差，且 float 只有 24 位尾数。
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

uint32_t ParticleGpuSpawnSeed(uint32_t emitter_seed, uint64_t particle_index)
{
    uint32_t h = ParticleGpuHashU32(emitter_seed ^ 0x9e3779b9u);
    h = ParticleGpuHashU32(h ^ static_cast<uint32_t>(particle_index & 0xFFFFFFFFu));
    h = ParticleGpuHashU32(h ^ static_cast<uint32_t>(particle_index >> 32));
    // 掩码到 24 位：种子的载体是 ParticleGpuSpawn::seed（float，因为出生记录要走
    // 实例属性通道，而那条通道没有 uint）。float 只能精确表示到 2^24，不掩码就会
    // 在写入 float 时悄悄截断——CPU 与 GPU 拿到的就不是一个种子了。着色器侧
    // `uint seed = uint(s.seed)` 因此是精确的，不需要 floatBitsToUint。
    return h & 0x00FFFFFFu;
}
