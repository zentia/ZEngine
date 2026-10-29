#include "ParticleGpuEmitter.h"

#include <cmath>
#include <cstddef>

bool ParticleGpuEmitterCheckCapacity(const ParticleGpuEmitterDesc& desc, uint32_t& required)
{
    if (desc.mode == static_cast<uint32_t>(ParticleGpuEmitMode::Burst))
    {
        required = desc.burstCount;
        return desc.capacity >= desc.burstCount;
    }
    const double need = static_cast<double>(desc.rate) * static_cast<double>(desc.lifetime);
    // +2：存活区间是半开半闭的（k > (t-life)*rate 且 k <= t*rate），两个端点各占
    // 一格；再 +1 是因为缓冲里还得留一个"下一个要写的槽"不能同时被读作存活。
    const double total = need + 2.0;
    required = static_cast<uint32_t>(std::ceil(total));
    return desc.capacity >= required;
}

void ParticleGpuEmitterReset(ParticleGpuEmitterState& state, const ParticleGpuEmitterDesc& desc)
{
    state.spawns.assign(static_cast<size_t>(desc.capacity), ParticleGpuSpawn {});
    state.capacity = desc.capacity;
    state.emitted = 0;
    state.time = 0.0f;
    state.burst_done = false;
    state.burst_first = 0;
    state.burst_count = 0;
}

uint32_t ParticleGpuRingSlot(uint64_t particle_index, uint32_t capacity)
{
    if (capacity == 0u)
    {
        return 0u;
    }
    return static_cast<uint32_t>(particle_index % static_cast<uint64_t>(capacity));
}

ParticleGpuSpawn ParticleGpuMakeSpawn(const ParticleGpuEmitterDesc& desc, uint64_t particle_index)
{
    ParticleGpuSpawn s {};
    const uint32_t seed = ParticleGpuSpawnSeed(desc.seed, particle_index);

    // 出生时刻只由序号决定 —— 这是整个 Tier-1 能 O(1) 调度的前提，也保证 dt 抖动
    // 不会影响发射节奏。Burst 是例外：整批同生，出生时刻都是 0。
    if (desc.mode == static_cast<uint32_t>(ParticleGpuEmitMode::Burst))
    {
        s.spawnTime = 0.0f;
    }
    else
    {
        s.spawnTime = static_cast<float>(static_cast<double>(particle_index) /
                                         static_cast<double>(desc.rate > 0.0f ? desc.rate : 1.0f));
    }

    // 位置：球面均匀撒点（简单实现：拒绝法会带来分支，这里用"三个独立随机器 +
    // 归一化"，偏差可接受且无循环，GPU/C# 镜像也就不必实现同样的拒绝序列）。
    uint32_t rng = seed;
    const float u1 = ParticleGpuRandom01(rng);
    const float u2 = ParticleGpuRandom01(rng);
    const float u3 = ParticleGpuRandom01(rng);
    float dir[3] = {u1 * 2.0f - 1.0f, u2 * 2.0f - 1.0f, u3 * 2.0f - 1.0f};
    const float len_sq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    const float inv = (len_sq > 1.0e-12f) ? (desc.spread / std::sqrt(len_sq)) : 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        s.p0[i] = desc.origin[i] + dir[i] * inv;
    }

    // 速度：初速 + 各轴独立扰动
    const float s1 = ParticleGpuRandom01(rng) * 2.0f - 1.0f;
    const float s2 = ParticleGpuRandom01(rng) * 2.0f - 1.0f;
    const float s3 = ParticleGpuRandom01(rng) * 2.0f - 1.0f;
    s.v0[0] = desc.velocity[0] + s1 * desc.velocitySpread;
    s.v0[1] = desc.velocity[1] + s2 * desc.velocitySpread;
    s.v0[2] = desc.velocity[2] + s3 * desc.velocitySpread;

    for (int i = 0; i < 3; ++i)
    {
        s.a[i] = desc.acceleration[i];
    }

    s.life = desc.lifetime;
    s.seed = static_cast<float>(seed);
    s.size0 = desc.startSize;
    s.sizeCurve = static_cast<float>(desc.sizeCurve);
    s.colorGradient = static_cast<float>(desc.colorGradient);
    s.flags = 0.0f;
    return s;
}

ParticleGpuWindow ParticleGpuEmitterTick(ParticleGpuEmitterState& state,
                                         const ParticleGpuEmitterDesc& desc,
                                         float time)
{
    ParticleGpuWindow window {};
    window.capacity = state.capacity;
    // 时间倒流（回放 / 重置）不做特殊处理：按新时间重算窗口，已写入的记录留着不
    // 动，下一次写入会覆盖。
    state.time = time;

    if (desc.mode == static_cast<uint32_t>(ParticleGpuEmitMode::Burst))
    {
        if (!state.burst_done && desc.burstCount > 0u)
        {
            const uint32_t n = (desc.burstCount > state.capacity) ? state.capacity : desc.burstCount;
            for (uint32_t i = 0u; i < n; ++i)
            {
                const uint64_t k = state.emitted;
                state.spawns[ParticleGpuRingSlot(k, state.capacity)] = ParticleGpuMakeSpawn(desc, k);
                ++state.emitted;
            }
            state.burst_first = ParticleGpuRingSlot(state.emitted - static_cast<uint64_t>(n), state.capacity);
            state.burst_count = n;
            state.burst_done = true;
        }
        // 整批同生同死：spawnTime 都是 0，寿命一到就全没。
        if (time <= desc.lifetime && desc.lifetime > 0.0f)
        {
            window.first = state.burst_first;
            window.count = state.burst_count;
        }
        return window;
    }

    // ---- 恒定速率 ----
    if (desc.rate <= 0.0f)
    {
        return window;
    }

    const double dtime = static_cast<double>(time);
    const double drate = static_cast<double>(desc.rate);

    // t 时刻在册的粒子是序号 0..floor(t*rate)，共 floor(t*rate)+1 个（第 k 个在
    // k/rate 出生，k=0 出生在 t=0）。
    const double scheduled_exact = dtime * drate;
    const uint64_t scheduled =
        (scheduled_exact < 0.0) ? 0ull : (static_cast<uint64_t>(std::floor(scheduled_exact)) + 1ull);

    if (scheduled > state.emitted)
    {
        // dt 巨大（切场景 / 卡顿）时不要真的写几百万条：只保留最后 capacity 个。
        const uint64_t missing = scheduled - state.emitted;
        const uint64_t skip = (missing > state.capacity) ? (missing - state.capacity) : 0ull;
        const uint64_t start = state.emitted + skip;
        for (uint64_t k = start; k < scheduled; ++k)
        {
            state.spawns[ParticleGpuRingSlot(k, state.capacity)] = ParticleGpuMakeSpawn(desc, k);
        }
        state.emitted = scheduled;
    }

    // 存活窗口：k ∈ ((t - life) * rate, t * rate]
    const double lo = (dtime - static_cast<double>(desc.lifetime)) * drate;
    uint64_t k_min = 0ull;
    if (lo > 0.0)
    {
        k_min = static_cast<uint64_t>(std::floor(lo)) + 1ull;
    }
    if (scheduled == 0ull)
    {
        window.count = 0u;
        return window;
    }
    // 已写入的最后一条就是序号 scheduled-1（无论这一帧有没有补写：emitted 在补写
    // 后必然等于 scheduled，时间倒流时取小的那个）。
    const uint64_t k_max = (state.emitted < scheduled) ? (state.emitted - 1ull) : (scheduled - 1ull);

    // 环形缓冲只留最后 capacity 个序号
    if (state.emitted > state.capacity)
    {
        const uint64_t first_kept = state.emitted - state.capacity;
        if (k_min < first_kept)
        {
            k_min = first_kept;
        }
    }
    if (k_max + 1ull < k_min)
    {
        window.count = 0u;
        return window;
    }
    uint64_t count = k_max - k_min + 1ull;
    if (count > state.capacity)
    {
        count = state.capacity;
    }

    window.first = ParticleGpuRingSlot(k_min, state.capacity);
    window.count = static_cast<uint32_t>(count);
    return window;
}
