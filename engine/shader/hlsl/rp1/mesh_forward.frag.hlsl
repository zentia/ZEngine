// DX-B3: forward transparent pass into backup_odd.

#include "../common/material_custom_params.hlsli"

// 布局必须与 CPU 侧 MeshMaterialUniform（Runtime/Function/Render/RenderCommon.h）
// 逐字节一致。padding 写成三个独立 uint 而不是 uint3 的原因见
// hlsl/common/render_storage_structs.hlsli 里的注释（uint3 会被整体挪到下一个
// 寄存器，customParams 会落到 80 而不是 64，和 GLSL 的 std140 布局错开）。
cbuffer PerMaterial : register(b0, space2)
{
    float4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float3 emissiveFactor;
    uint is_blend;
    uint is_double_sided;
    uint _padding_0;
    uint _padding_1;
    uint _padding_2;
    float4 customParams[4];
};

Texture2D base_color_texture : register(t1, space2);
SamplerState base_color_sampler : register(s1, space2);

struct PsInput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

float4 main(PsInput input) : SV_Target0
{
    // 溶解（动态属性块 customParams[0]/[1]）。没开溶解时读完 amount 就返回。
    // HLSL 的 discard 不像 GLSL 那样终止执行，所以后面补一个 return。
    float3 dissolve_edge = float3(0.0f, 0.0f, 0.0f);
    if (ZMaterialDissolveCull(input.world_position, customParams[0], customParams[1], dissolve_edge))
    {
        discard;
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float4 base = base_color_texture.Sample(base_color_sampler, input.texcoord) * baseColorFactor;
    // 前向通道直接写颜色，溶解边缘按自发光加上去（不参与光照）。
    return base + float4(dissolve_edge, 0.0f);
}
