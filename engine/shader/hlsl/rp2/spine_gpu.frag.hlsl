// GPU-Spine 片元（DX12 版）—— 与 glsl/spine_gpu.frag **同一套公式**。
// 着色公式本身在 include/spine_skinning.h 的 `SpineShade()`（与 GLSL 版共用同一份文件），
// 这里只负责取址（读图集、读绘制项）与传参。
// flags 语义见 spine_skinning.h 的 SPINE_FLAG_*：bit0 直 alpha 图集先预乘、bit1 加色（alpha 写 0）、
// bit4 tint black、bit5 世界层（去预乘 → pow 2.2 → 再预乘）。
// 寄存器映射见 spine_gpu.vert.hlsl 文件头。

#define ZENGINE_HLSL 1
#include "spine_skinning.h"

struct SpineDraw
{
    uint vertex_format;
    uint uv_offset;
    uint slot_bone;
    uint bone_base;
    uint inst_index;
    uint tint;
    uint flags;
    uint weighted_base;
    uint deform_base;
    float dark_r;
    float dark_g;
    float dark_b;
};

StructuredBuffer<SpineDraw> draws : register(t5, space0);
Texture2D u_atlas : register(t0, space1);
SamplerState u_atlas_sampler : register(s0, space1);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint draw : TEXCOORD1;
};

// = GLSL unpackUnorm4x8：x 在最低字节
float4 UnpackUnorm4x8(uint v)
{
    return float4(float(v & 0xFFu), float((v >> 8u) & 0xFFu), float((v >> 16u) & 0xFFu), float(v >> 24u)) / 255.0;
}

float4 main(PSInput input) : SV_TARGET
{
    const SpineDraw d = draws[input.draw];
    // 槽位色是直 alpha（§5.2）⇒ 预乘形式在 SpineShade() 内部做
    const float4 tint = UnpackUnorm4x8(d.tint);
    return SpineShade(u_atlas.Sample(u_atlas_sampler, input.uv), tint,
                      float3(d.dark_r, d.dark_g, d.dark_b), d.flags);
}
