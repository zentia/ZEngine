#pragma once

// =====================================================================
// 拼包 + 顶点着色器的 CPU 镜像
// ---------------------------------------------------------------------
// 与 SpineGpuBatch 同一条边界：**只做 CPU 侧的拼包，不碰 RHI**。适配器（ZEngine
// 的 ParticlePass / Unity 的渲染器）只做两件事——上传这些 POD，按 cmd 发一次
// 实例化绘制。于是"偏置算错没有""哪几个粒子活着"都能在没有 GPU 的单测里验证。
//
// ParticleGpuEmulateSpawn() 是 particle_sim.vert 的逐行 CPU 版（第 4 步落地那个
// 着色器）：同一运算顺序、同样的越界判定。改公式必须两边一起改，跨语言对拍会抓。
// =====================================================================

#include "ParticleGpuEmitter.h"
#include "ParticleGpuFormat.h"

#include <cstdint>
#include <vector>

struct ParticleGpuBatch
{
    // 所有发射器的出生记录首尾相接拼在一起（每个发射器 capacity 条，DrawCmd 里的
    // first 已经是它在合包里的**全局**下标）。
    std::vector<ParticleGpuSpawn> spawns;
    // 每个发射器一段：适配器按 cmd 发 CmdDrawInstanced(cmd.count)。
    std::vector<ParticleGpuDrawCmd> cmds;
};

// 把一个发射器的环形缓冲追加进合包，返回它在合包里的段下标（-1 = 容量非法）。
// 追加而不是拷贝整个环：稳态下每帧只是"把没写过的槽补上"，适配器用增量上传即可。
int32_t ParticleGpuAppendEmitter(ParticleGpuBatch& batch,
                                 const ParticleGpuEmitterState& state,
                                 const ParticleGpuWindow& window);

// ---------------------------------------------------------------------
// 顶点着色器（Tier-1）的 CPU 镜像
// ---------------------------------------------------------------------
struct ParticleGpuEmulated
{
    float position[3];
    float age;        // 归一化年龄 t / life ∈ [0,1]，供曲线 / 渐变采样
    float life;
    bool alive;
};

// 闭式积分：pos = p0 + v0*t + 0.5*a*t²。
// 返回 alive=false 表示"未出生或已死"——着色器侧对应把顶点退化掉（或由 CPU 给的
// count 直接排除，正常路径下不会走到）。
ParticleGpuEmulated ParticleGpuEmulateSpawn(const ParticleGpuSpawn& spawn, float time);
