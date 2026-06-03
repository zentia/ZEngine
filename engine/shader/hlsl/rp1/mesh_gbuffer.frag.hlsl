// DX-B3: gbuffer MRT output (layout matches shader/include/gbuffer.h).

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

Texture2D metallic_roughness_texture : register(t2, space2);
SamplerState metallic_roughness_sampler : register(s2, space2);

Texture2D normal_texture : register(t3, space2);
SamplerState normal_sampler : register(s3, space2);

struct PsInput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

struct PsOutput
{
    float4 gbuffer_a : SV_Target0;
    float4 gbuffer_b : SV_Target1;
    float4 gbuffer_c : SV_Target2;
};

float3 EncodeNormal(float3 n)
{
    return n * 0.5f + 0.5f;
}

PsOutput main(PsInput input)
{
    float3 albedo = base_color_texture.Sample(base_color_sampler, input.texcoord).rgb * baseColorFactor.rgb;
    float4 mr = metallic_roughness_texture.Sample(metallic_roughness_sampler, input.texcoord);
    float metallic = mr.b * metallicFactor;
    float roughness = mr.g * roughnessFactor;

    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3 tangent_normal = normal_texture.Sample(normal_sampler, input.texcoord).xyz * 2.0f - 1.0f;
    tangent_normal.xy *= normalScale;
    N = normalize(mul(float3x3(T, B, N), tangent_normal));

    PsOutput output;
    output.gbuffer_a = float4(EncodeNormal(N), 1.0f);
    output.gbuffer_b = float4(metallic, 0.5f, roughness, 1.0f / 255.0f);
    output.gbuffer_c = float4(albedo, 1.0f);
    return output;
}
