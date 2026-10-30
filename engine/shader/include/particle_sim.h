// ---------------------------------------------------------------------------
// GPUParticle 公式层 —— 唯一权威
//
// 这里**只有公式**：不声明任何 buffer / 纹理 / 寄存器，不做任何取址。
// 数据全部由调用方喂进来。因此同一份文件能被不同语言、不同通道同时消费：
//   * glsl/particle_gpu.vert       —— glslangValidator，-I 本目录
//   * hlsl/rp2/particle_gpu.vert.hlsl —— DXC，include 前须 `#define ZENGINE_HLSL 1`
//   * Unity 侧（URP / HLSL）—— 同样 define 后 include，只换取址
//   * 去 SSBO 的纹理通道（出生记录走 VTF）—— 只换取址，公式不动
//
// 语言分支沿用 spine_skinning.h 的约定：HLSL 侧定义 ZENGINE_HLSL，GLSL 侧不定义。
// ⇒ **HLSL 消费者必须在 #include 之前写 `#define ZENGINE_HLSL 1`**；
//   忘了会拿到 vec2 分支而编译失败 —— 这是有意的响亮失败，好过悄悄拿错类型。
//
// ⚠️ 改这里 = 同时改每端的取址文件 + CPU 参考实现
//    （External/GPUParticle 的 ParticleGpuCurves.cpp / ParticleGpuEmitter.cpp，
//      以及将要补的 ParticleGpuEmulateVertex）。
//    **不要顺手重排运算顺序**：f32 加法不满足结合律，重排能编译、能跑，
//    只是结果静默地变 —— 这正是本文件存在的理由。
//
// 规格：doc/GPU_PARTICLE_DESIGN.md；容器布局：ParticleGpuFormat.h
// ---------------------------------------------------------------------------

#ifndef ZENGINE_SHADER_INCLUDE_PARTICLE_SIM_H
#define ZENGINE_SHADER_INCLUDE_PARTICLE_SIM_H

// ---- 语言方言 ------------------------------------------------------------
#ifdef ZENGINE_HLSL
#define PARTICLE_FLOAT2 float2
#define PARTICLE_FLOAT3 float3
#define PARTICLE_FLOAT4 float4
#define PARTICLE_UINT   uint
#define PARTICLE_INT    int
#define PARTICLE_PRECISE precise
#else
#define PARTICLE_FLOAT2 vec2
#define PARTICLE_FLOAT3 vec3
#define PARTICLE_FLOAT4 vec4
#define PARTICLE_UINT   uint
#define PARTICLE_INT    int
#define PARTICLE_PRECISE precise
#endif

// ---- 曲线模式（= ParticleGpuCurveMode，ParticleGpuFormat.h:28-35）----------
#define PARTICLE_CURVE_MODE_CONSTANT                0u
#define PARTICLE_CURVE_MODE_CURVE                   1u
#define PARTICLE_CURVE_MODE_RANDOM_TWO_CONSTANTS    2u
#define PARTICLE_CURVE_MODE_RANDOM_TWO_CURVES       3u

// ---- 发射器模式（= ParticleGpuEmitMode）-----------------------------------
#define PARTICLE_EMIT_MODE_CONSTANT_RATE 0u
#define PARTICLE_EMIT_MODE_BURST         1u

// ===========================================================================
// 1) 确定性随机
// ===========================================================================
// lowbias32（Chris Wellons），逐行照抄 ParticleGpuCurves.cpp:172-199。
// 只要 uint 的溢出是 wrap 的，任何语言都能复现 —— GLSL 与 HLSL 都是 wrap，
// 但 **GLSL 必须标 highp**：移动端默认精度下 uint 乘法可能被降级，随机序列
// 就变了，表现是"粒子分布和编辑器里不一样"这种最难查的静默差异。
//
// C# 侧必须放在 unchecked 上下文里，否则 OverflowException。
PARTICLE_UINT particle_hash_u32(PARTICLE_UINT x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 原地推进 state 并返回 [0,1)。取高 24 位：低位在乘法哈希里质量差，
// 且 f32 只有 24 位尾数。
float particle_random01(inout PARTICLE_UINT state)
{
    state = particle_hash_u32(state);
    return float(state >> 8) * (1.0 / 16777216.0);
}

// ===========================================================================
// 2) 闭式解（Tier-1 无状态粒子的全部运动学）
// ===========================================================================
// 出生那一刻之后**没有任何状态**：位置与速度都只由 (p0, v0, a, t) 决定，
// 不需要逐帧积分，因此不会有浮点漂移，也不需要回写任何缓冲。
//
// ⚠️ 运算顺序与 ParticleGpuEmitter.cpp 的 CPU 版逐字一致：
//    p0 + v0*t + 0.5*a*t*t，不要写成 p0 + t*(v0 + 0.5*a*t)。
PARTICLE_FLOAT3 particle_position(PARTICLE_FLOAT3 p0, PARTICLE_FLOAT3 v0, PARTICLE_FLOAT3 a, float t)
{
    return p0 + v0 * t + 0.5 * a * t * t;
}

PARTICLE_FLOAT3 particle_velocity(PARTICLE_FLOAT3 v0, PARTICLE_FLOAT3 a, float t)
{
    return v0 + a * t;
}

// ===========================================================================
// 3) 年龄
// ===========================================================================
// 归一化 [0,1)。出生前为负、死后 >1 ⇒ 调用方用 particle_is_alive 判存活，
// 而不是靠"有没有这条记录"——环形缓冲里的槽位永远是满的。
float particle_age(float now, float spawn_time, float life)
{
    // life 已在烘焙期保证 > 0（ParticleGpuValidateContainer 校验）
    return (now - spawn_time) / life;
}

bool particle_is_alive(float age)
{
    return age >= 0.0 && age < 1.0;
}

// ===========================================================================
// 4) 曲线求值
// ===========================================================================
// 关键：烘焙期已把任意曲线重采样成**等距键**（ParticleGpuResampleCurve，
// time = i / (keyCount - 1)），所以这里不需要关键帧查找，一次取整 + lerp 就够。
//
// 与 CPU 版（ParticleGpuEvaluateCurve 的线性扫）逐段一致：
//   * t 落在两个键之间 ⇒ 两边都取同一个下标，frac 相同；
//   * t 恰好等于某个键的时间 ⇒ CPU 的 `times[i+1] <= t` 会前进到该下标、
//     frac = 0，GPU 的 floor 也给出该下标、frac = 0 ⇒ 都返回该键的值。
//
// ⚠️ 这条 O(1) 路径**只在键等距时成立**。容器里若出现非等距键（手写容器），
//    两端会静默地取到不同的段 —— 所以烘焙器必须用 ParticleGpuResampleCurve，
//    不要用别的重采样方式。
void particle_curve_locate(float t, PARTICLE_INT key_count, out PARTICLE_INT i0, out float frac)
{
    // 键数 < 2（畸形容器）：退化成取第 0 个键，不产生 NaN，也不越界。
    // 烘焙期 ParticleGpuValidateContainer 已经挡过 keyCount == 0，这里是兜底。
    if (key_count < 2)
    {
        i0 = 0;
        frac = 0.0;
        return;
    }

    float clamped = clamp(t, 0.0, 1.0);
    float u = clamped * float(key_count - 1);
    PARTICLE_INT i = PARTICLE_INT(floor(u));
    // 最后一个键之后：钳到倒数第二段，frac = 1（等价于取末键值）
    i = clamp(i, 0, key_count - 2);
    i0 = i;
    frac = u - float(i);
    if (frac > 1.0)
    {
        frac = 1.0;
    }
}

// 一段的两个值 + frac → 插值结果。单独抽出来是为了让"取址"与"插值"分开，
// 于是 VTF 通道与 SSBO 通道共用同一个插值式。
float particle_curve_lerp(float a, float b, float frac)
{
    return a + (b - a) * frac;
}

// 按 CurveRange 的模式出最终值。`curve_value` / `curve_value_max` 由调用方取好：
//   * Curve                 ⇒ 只用 curve_value
//   * RandomBetweenTwoCurves⇒ 两条曲线各取一次（键是紧挨着的两段，
//                             见 ParticleGpuEvaluateCurveRange 的注释）
//   * 两种常量模式          ⇒ 两者都不需要，传 0 即可
// 空曲线（key_count == 0）按常量 1 处理：乘法中性，不会把粒子变成 0 尺寸。
float particle_curve_range_eval(PARTICLE_UINT mode, float constant_min, float constant_max, float random01,
                                float curve_value, float curve_value_max)
{
    if (mode == PARTICLE_CURVE_MODE_CONSTANT)
    {
        return constant_min;
    }
    if (mode == PARTICLE_CURVE_MODE_CURVE)
    {
        return curve_value;
    }
    if (mode == PARTICLE_CURVE_MODE_RANDOM_TWO_CONSTANTS)
    {
        return constant_min + (constant_max - constant_min) * random01;
    }
    if (mode == PARTICLE_CURVE_MODE_RANDOM_TWO_CURVES)
    {
        return curve_value + (curve_value_max - curve_value) * random01;
    }
    return 1.0;
}

// ===========================================================================
// 5) 渐变求值
// ===========================================================================
// 语义对齐 Unity 的 Gradient：颜色键与 alpha 键**各自独立**插值，两端夹紧
// （GradientMode 默认 Blend：超出范围取端点，不循环、不乒乓）。
// 与曲线同理，烘焙期已重采样成等距键 ⇒ 复用 particle_curve_locate。

PARTICLE_FLOAT3 particle_gradient_color(PARTICLE_FLOAT3 ca, PARTICLE_FLOAT3 cb, float frac)
{
    return ca + (cb - ca) * frac;
}

float particle_gradient_alpha(float aa, float ab, float frac)
{
    return aa + (ab - aa) * frac;
}

// ===========================================================================
// 6) 尺寸
// ===========================================================================
// 出生尺寸 × 尺寸曲线。曲线为空时 curve 值是 1 ⇒ 恒等于 size0。
float particle_size(float size0, float size_curve_value)
{
    return size0 * size_curve_value;
}

// ===========================================================================
// 7) 槽位（环形缓冲寻址）
// ===========================================================================
// 存活窗口是环形的一段：slot = (first + vertexID/4) % capacity。
// 与 ParticleGpuDrawCmd 一致（first / count / capacity）。
//
// 这里是**纯整数**运算，不能先换成 float 再取模 —— f32 只有 24 位尾数，
// 大 capacity 下会取到错误的槽位（表现为"粒子随机跳到别人的位置"）。
PARTICLE_UINT particle_ring_slot(PARTICLE_UINT first, PARTICLE_UINT quad_index, PARTICLE_UINT capacity)
{
    return (first + quad_index) % capacity;
}

#endif  // ZENGINE_SHADER_INCLUDE_PARTICLE_SIM_H
