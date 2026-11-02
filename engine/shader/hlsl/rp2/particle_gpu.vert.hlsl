// ---------------------------------------------------------------------------
// GPU 粒子：顶点着色器（DX12 / DXC）
//
// 这个文件**只做取址 + 把公式串起来**，公式本身全在 particle_sim.h。
// 与 CPU 参考实现 ParticleGpuEmulateVertex（GPUParticle/ParticleGpuEmulate.cpp）
// 是逐行对应的：编号 0..10 与那边一一对应，改任一边请对着改另一边。
//
// 顶点不从顶点缓冲来：quad 角与 quad 下标都由 SV_VertexID 推
//   corner = vertexID & 3      (= ParticleGpuQuadCorner 的角序)
//   quad   = vertexID >> 2     (= 存活窗口里的第几个粒子)
// 与 GPU-Spine 在 Vulkan / Metal 上"无顶点流、全从 SSBO 取"的做法同款；
// 适配层仍会绑一个共享 quad VB（属性通道用得上），DX12 允许布局多声明。
//
// set 0 绑定（== ParticleGpuDrawConsts / ParticleGpuRenderer.cpp 的那几块）：
//   b0 spawns          StructuredBuffer<Spawn>        出生记录，环形整条
//   b1 consts          StructuredBuffer<DrawConsts>   本绘制常量（1 个元素）
//   b2 emitter_data    ByteAddressBuffer              发射器数据块（见下）
// ---------------------------------------------------------------------------

#define ZENGINE_HLSL 1   // 必须写在 include 之前：particle_sim.h 靠它选方言
#include "particle_sim.h"

// ---- 取址（本文件唯一允许出现"数据长什么样"的地方）------------------------

// 出生记录 = 4 x vec4 = 64 B（ParticleGpuSpawn，ParticleGpuFormat.h:109）
struct Spawn
{
    float4 r0;   // p0.xyz, spawnTime
    float4 r1;   // v0.xyz, life
    float4 r2;   // a.xyz,  seed
    float4 r3;   // size0, sizeCurve, colorGradient, flags
};

// ParticleGpuDrawConsts（ParticleGpuRenderer.cpp 同名结构）
struct DrawConsts
{
    float4x4 view_proj;
    float4x4 model;
    float4 cam_right;   // xyz + 占位
    float4 cam_up;      // xyz + 占位
    float time;
    uint window_first;
    uint window_count;
    uint capacity;
    // 发射器数据块（b2）里各段的**字节偏移**
    uint curve_range_off;
    uint curve_key_off;
    uint grad_range_off;
    uint grad_color_off;
    uint grad_alpha_off;
    // 各段元素数（越界读的兜底）
    uint curve_range_count;
    uint curve_key_count;
    uint grad_range_count;
    uint grad_color_count;
    uint grad_alpha_count;
    uint pad0;
    uint pad1;
};

struct VSOutput
{
    float4 clip : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

StructuredBuffer<Spawn> spawns : register(t0);
StructuredBuffer<DrawConsts> consts : register(t1);
ByteAddressBuffer emitter_data : register(t2);

// ---- b2 里的小结构（与 ParticleGpuFormat.h 同款，用字节地址读是因为
//      ParticleGpuCurveRange 只有 20 B，凑不进 16 B 对齐的 StructuredBuffer）----
// ParticleGpuCurveRange: mode u32, keyOffset u32, keyCount u32, constantMin f32, constantMax f32
// ParticleGpuCurveKey:   time f32, value f32
// ParticleGpuGradientRange: colorKeyOffset, colorKeyCount, alphaKeyOffset, alphaKeyCount (4 x u32)
// ParticleGpuGradientColorKey: time, r, g, b (4 x f32)
// ParticleGpuGradientAlphaKey: time, a (2 x f32)

VSOutput main(uint vertex_id : SV_VertexID)
{
    VSOutput o;
    o.clip = float4(0.0, 0.0, 0.0, 0.0);
    o.uv = float2(0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);

    const DrawConsts c = consts[0];

    const uint corner = vertex_id & 3u;   // = ParticleGpuQuadCorner 的角序
    const uint quad = vertex_id >> 2u;    // 存活窗口内的第几个粒子

    // ---- 1) 槽位 = particle_ring_slot（纯整数取模，不能先转 float）----
    const uint slot = particle_ring_slot(c.window_first, quad, c.capacity);
    const Spawn s = spawns[slot];

    const float3 p0 = s.r0.xyz;
    const float spawn_time = s.r0.w;
    const float3 v0 = s.r1.xyz;
    const float life = s.r1.w;
    const float3 a = s.r2.xyz;
    const float seed = s.r2.w;
    const float size0 = s.r3.x;
    const uint size_curve_index = asuint(s.r3.y);
    const uint gradient_index = asuint(s.r3.z);

    // ---- 2) 年龄 = particle_age / particle_is_alive ----
    const float t = c.time - spawn_time;
    const float age = particle_age(c.time, spawn_time, life);
    if (!particle_is_alive(age))
    {
        // 不该发生：Draw 的 count 只含存活粒子。退化成"画在原点、完全透明"，
        // 至少不会把已经死掉的粒子画成噪声。
        return o;
    }

    // ---- 3) 随机 = particle_hash_u32 / particle_random01 ----
    uint rnd_state = asuint(seed);
    const float random01 = particle_random01(rnd_state);

    // ---- 4) 尺寸 = particle_curve_range_eval + particle_size ----
    // 读 CurveRange（20 B：4 u32 + 2 f32 里的前 1 个 f32 在下一格边界上）
    const uint cr_off = c.curve_range_off + size_curve_index * 20u;
    const uint cr_mode = emitter_data.Load(cr_off + 0u);
    const uint cr_key_offset = emitter_data.Load(cr_off + 4u);
    const uint cr_key_count = emitter_data.Load(cr_off + 8u);
    const float cr_const_min = asfloat(emitter_data.Load(cr_off + 12u));
    const float cr_const_max = asfloat(emitter_data.Load(cr_off + 16u));

    float curve_value = 1.0;
    float curve_value_max = 1.0;
    if (cr_key_count >= 2u)
    {
        int i0 = 0;
        float frac = 0.0;
        particle_curve_locate(age, int(cr_key_count), i0, frac);
        const uint k0_off = c.curve_key_off + (cr_key_offset + uint(i0)) * 8u;
        const float k0 = asfloat(emitter_data.Load(k0_off + 4u));
        const float k1 = asfloat(emitter_data.Load(k0_off + 8u + 4u));
        curve_value = particle_curve_lerp(k0, k1, frac);
        // RandomBetweenTwoCurves：两条曲线的键紧挨着（CPU 版注释同款）
        const uint second = cr_key_offset + cr_key_count;
        int j0 = 0;
        float jfrac = 0.0;
        particle_curve_locate(age, int(cr_key_count), j0, jfrac);
        const uint k1_off = c.curve_key_off + (second + uint(j0)) * 8u;
        const float m0 = asfloat(emitter_data.Load(k1_off + 4u));
        const float m1 = asfloat(emitter_data.Load(k1_off + 8u + 4u));
        curve_value_max = particle_curve_lerp(m0, m1, jfrac);
    }
    const float size_scale =
        particle_curve_range_eval(cr_mode, cr_const_min, cr_const_max, random01, curve_value, curve_value_max);
    const float size = particle_size(size0, size_scale);

    // ---- 5) 颜色 = particle_gradient_color / particle_gradient_alpha ----
    float3 rgb = float3(1.0, 1.0, 1.0);
    float alpha = 1.0;
    if (gradient_index < c.grad_range_count)
    {
        const uint gr_off = c.grad_range_off + gradient_index * 16u;
        const uint g_color_off = emitter_data.Load(gr_off + 0u);
        const uint g_color_count = emitter_data.Load(gr_off + 4u);
        const uint g_alpha_off = emitter_data.Load(gr_off + 8u);
        const uint g_alpha_count = emitter_data.Load(gr_off + 12u);

        if (g_color_count >= 2u)
        {
            int i0 = 0;
            float frac = 0.0;
            particle_curve_locate(age, int(g_color_count), i0, frac);
            // color key 正好 16 B（time, r, g, b）⇒ 整格 Load4，取 .yzw 跳过 time。
            // 不能写 Load4(k0 + 4)：ByteAddressBuffer.Load4 要求偏移 16 B 对齐。
            const uint k0 = c.grad_color_off + (g_color_off + uint(i0)) * 16u;
            const float3 ca = asfloat(emitter_data.Load4(k0)).yzw;
            const float3 cb = asfloat(emitter_data.Load4(k0 + 16u)).yzw;
            rgb = particle_gradient_color(ca, cb, frac);
        }
        if (g_alpha_count >= 2u)
        {
            int i0 = 0;
            float frac = 0.0;
            particle_curve_locate(age, int(g_alpha_count), i0, frac);
            const uint k0 = c.grad_alpha_off + (g_alpha_off + uint(i0)) * 8u;
            const float aa = asfloat(emitter_data.Load(k0 + 4u));
            const float ab = asfloat(emitter_data.Load(k0 + 8u + 4u));
            alpha = particle_gradient_alpha(aa, ab, frac);
        }
    }

    // ---- 6) 位置 = particle_position（闭式解，别重排）+ 发射器原点 ----
    // CPU 版（ParticleGpuEmulate.cpp:209-211）在闭式解之后加 desc->origin：
    // 发射器的 origin 是作者数据、不随粒子变，所以不进出生记录，加在这里。
    // ParticleGpuEmitterDesc 96 B，前 3 个 float 就是 origin（ParticleGpuFormat.h:82）。
    const float3 origin = asfloat(emitter_data.Load3(0u));
    const float3 local = particle_position(p0, v0, a, t) + origin;

    // ---- 7) 世界：model × 局部点 ----
    const float3 world = mul(c.model, float4(local, 1.0)).xyz;

    // ---- 8) 公告板角偏移 = particle_corner_offset ----
    const float2 co = particle_corner_offset(int(corner));
    const float3 world_pos = world + c.cam_right.xyz * co.x * size + c.cam_up.xyz * co.y * size;

    // ---- 9) 裁剪：view_proj × 世界点 ----
    o.clip = mul(c.view_proj, float4(world_pos, 1.0));

    // ---- 10) UV = particle_corner_uv ----
    o.uv = particle_corner_uv(int(corner));
    o.color = float4(rgb, alpha);
    return o;
}
