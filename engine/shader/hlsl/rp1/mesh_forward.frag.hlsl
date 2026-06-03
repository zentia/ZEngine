// DX-B3: forward transparent pass into backup_odd.

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
    float4 base = base_color_texture.Sample(base_color_sampler, input.texcoord) * baseColorFactor;
    return base;
}
