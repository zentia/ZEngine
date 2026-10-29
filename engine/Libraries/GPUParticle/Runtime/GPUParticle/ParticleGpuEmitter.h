#pragma once

// =====================================================================
// 发射调度（Tier-1 无状态粒子的 CPU 半边）
// ---------------------------------------------------------------------
// Tier-1 的关键：粒子**没有状态**。每个粒子在出生那一刻被烘焙成一条
// ParticleGpuSpawn，之后 GPU 每帧按闭式解
//
//     t     = time - spawnTime
//     pos   = p0 + v0 * t + 0.5 * a * t²
//
// 算出位置。于是 emit / simulate / kickoff 三条 compute pass、alive/dead
// 链表、counter buffer、indirect dispatch 参数**全部不需要**——这正是它能在
// 没有 compute shader 的机型（GLES3.0 / WebGL2）上跑的原因。
//
// CPU 剩下的活只有"决定这一帧谁活着"，而恒定速率发射器的发射时刻是可以闭式
// 算出来的（第 k 个粒子在 k/rate 秒出生），所以：
//
//   - 发射：已知当前 time，缺哪几个序号就补哪几个，与 dt 无关，不会有浮点累积
//     漂移（用 `carry += rate*dt` 那种写法，帧率一变发射节奏就跟着变）。
//   - 存活窗口：k ∈ ((time-life)*rate, time*rate]，O(1) 取整即可，不必遍历。
//
// 代价是"每帧要画多少"由 CPU 给出 —— 但没有 indirect dispatch 也照样能画，
// 因为实例数是 CPU 已知的：CmdDrawInstanced(count) 即可。
// =====================================================================

#include "ParticleGpuCurves.h"
#include "ParticleGpuFormat.h"

#include <cstdint>
#include <vector>

struct ParticleGpuEmitterState
{
    // 环形缓冲。容量 = desc.capacity；写满后覆盖最老的（那时最老的必然已死，
    // 只要 capacity >= rate * life，见 ParticleGpuEmitterCheckCapacity）。
    std::vector<ParticleGpuSpawn> spawns;

    uint32_t capacity {0};
    // 已"排过班"的粒子总数（单调递增，含已被覆盖的）。第 k 个粒子的出生时刻
    // 恒等于 k / rate，所以它是重建任意时刻存活窗口的唯一依据。
    uint64_t emitted {0};
    float time {0.0f};
    bool burst_done {false};
    uint32_t burst_first {0};
    uint32_t burst_count {0};
};

// 容量够不够：环形缓冲必须在"最老的那个还活着"之前不被覆盖。
// 需要 capacity >= ceil(rate * lifetime) + 1（+1 是同一个时刻的边界）。
bool ParticleGpuEmitterCheckCapacity(const ParticleGpuEmitterDesc& desc, uint32_t& required);

void ParticleGpuEmitterReset(ParticleGpuEmitterState& state, const ParticleGpuEmitterDesc& desc);

// 推进到 `time`（绝对时间，秒），补齐缺失的出生记录，返回这一帧要画的窗口。
//
// `first` 是存活窗口在环形缓冲里的起始槽位，`count` 是实例数。注意窗口在环形
// 缓冲里**可能绕回**：适配器要么把缓冲写两遍（属性通道，无取模），要么在 VS 里
// `slot = (first + instanceID) % capacity`（SSBO / VTF 通道）。
struct ParticleGpuWindow
{
    uint32_t first {0};
    uint32_t count {0};
    uint32_t capacity {0};
};

ParticleGpuWindow ParticleGpuEmitterTick(ParticleGpuEmitterState& state,
                                         const ParticleGpuEmitterDesc& desc,
                                         float time);

// 第 k 个粒子在环形缓冲里的槽位。
uint32_t ParticleGpuRingSlot(uint64_t particle_index, uint32_t capacity);

// ---------------------------------------------------------------------
// 出生记录的填充：把 desc + 序号 → 一条 ParticleGpuSpawn
// ---------------------------------------------------------------------
// 随机全部由 (emitter.seed, k) 决定，与发射顺序、帧率无关：跳过 / 补发都不会
// 改变任何一个粒子的取值，CPU 与 GPU 也必然一致。
ParticleGpuSpawn ParticleGpuMakeSpawn(const ParticleGpuEmitterDesc& desc, uint64_t particle_index);
