#include "ParticleGpuBatch.h"

#include <cstddef>

int32_t ParticleGpuAppendEmitter(ParticleGpuBatch& batch,
                                 const ParticleGpuEmitterState& state,
                                 const ParticleGpuWindow& window)
{
    if (state.spawns.empty())
    {
        return -1;
    }

    const uint32_t base = static_cast<uint32_t>(batch.spawns.size());
    batch.spawns.insert(batch.spawns.end(), state.spawns.begin(), state.spawns.end());

    ParticleGpuDrawCmd cmd {};
    cmd.first = base + window.first;
    cmd.count = window.count;
    cmd.capacity = state.capacity;
    cmd.flags = 0u;
    batch.cmds.push_back(cmd);
    return static_cast<int32_t>(batch.cmds.size() - 1);
}

ParticleGpuEmulated ParticleGpuEmulateSpawn(const ParticleGpuSpawn& spawn, float time)
{
    ParticleGpuEmulated out {};
    out.alive = false;
    out.age = 0.0f;
    out.life = spawn.life;
    for (int i = 0; i < 3; ++i)
    {
        out.position[i] = spawn.p0[i];
    }

    const float t = time - spawn.spawnTime;
    if (t < 0.0f || t > spawn.life)
    {
        return out;  // 未出生 / 已死
    }

    // 与着色器逐行对应：先算 t，再算归一化年龄，最后闭式积分。
    // 注意 0.5*a*t*t 的写法（不是 0.5*(a*t)*t 之外的结合顺序）——float 下结合顺序
    // 会改变舍入，CPU 与 GPU 必须同序，否则逐位对拍会差 1 ulp。
    out.age = (spawn.life > 0.0f) ? (t / spawn.life) : 0.0f;
    out.position[0] = spawn.p0[0] + spawn.v0[0] * t + 0.5f * spawn.a[0] * t * t;
    out.position[1] = spawn.p0[1] + spawn.v0[1] * t + 0.5f * spawn.a[1] * t * t;
    out.position[2] = spawn.p0[2] + spawn.v0[2] * t + 0.5f * spawn.a[2] * t * t;
    out.alive = true;
    return out;
}
