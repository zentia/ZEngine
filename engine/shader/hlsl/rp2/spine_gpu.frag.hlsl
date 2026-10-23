// GPU-Spine 片元（DX12 版）—— 与 glsl/spine_gpu.frag 同一套公式，改这边必须同步改那边。
// 输出预乘 alpha；flags bit0 直 alpha 图集先预乘、bit1 加色（alpha 写 0）、bit4 tint black。
// 寄存器映射见 spine_gpu.vert.hlsl 文件头。

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
    float4 tex = u_atlas.Sample(u_atlas_sampler, input.uv);
    if ((d.flags & 1u) != 0u)
    {
        tex.rgb *= tex.a;
    }
    const float4 tint = UnpackUnorm4x8(d.tint);
    float4 c = tex * float4(tint.rgb * tint.a, tint.a);
    if ((d.flags & 16u) != 0u)
    {
        c.rgb += (float3(1.0, 1.0, 1.0) - tex.rgb) * float3(d.dark_r, d.dark_g, d.dark_b) * (tex.a * tint.a);
    }
    // bit5 = 世界层（线性 HDR 场景）：去预乘 → pow 2.2 → 再预乘；与 spine_gpu.frag 逐行同公式
    if ((d.flags & 32u) != 0u && c.a > 0.0)
    {
        c.rgb = pow(max(c.rgb / c.a, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2)) * c.a;
    }
    if ((d.flags & 2u) != 0u)
    {
        c.a = 0.0;
    }
    return c;
}
