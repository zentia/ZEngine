// DX-B3: deferred lighting into backup_odd (subpass input attachments).
// DX12: sky HDR is written by Sky Pass (DrawSkyboxInRp1Forward) before this shader runs.
// UNLIT pixels discard so deferred lighting does not overwrite the cubemap.
// (matches Vulkan deferred_lighting.frag and DX12 MegaLights path).

#include "../common/scene_lighting_structs.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

#include "../common/main_camera_per_frame_access.hlsli"
#include "../common/hlsl_math.hlsli"

Texture2D brdf_lut : register(t3);
SamplerState brdf_sampler : register(s3);
TextureCube irradiance_map : register(t4);
SamplerState irradiance_sampler : register(s4);
TextureCube specular_map : register(t5);
SamplerState specular_sampler : register(s5);
Texture2DArray point_light_shadow : register(t6);
SamplerState point_shadow_sampler : register(s6);
Texture2D directional_light_shadow : register(t7);
SamplerState directional_shadow_sampler : register(s7);

Texture2D in_gbuffer_a : register(t0, space1);
Texture2D in_gbuffer_b : register(t1, space1);
Texture2D in_gbuffer_c : register(t2, space1);
Texture2D in_scene_depth : register(t3, space1);

#define SHADINGMODELID_UNLIT 0u
#define SHADINGMODELID_DEFAULT_LIT 1u

struct PsInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

float3 DecodeNormal(float3 packed)
{
    return normalize(packed * 2.0f - 1.0f);
}

uint DecodeShadingModelId(float packed_channel)
{
    return (uint)round(packed_channel * 255.0f);
}

float4 main(PsInput input) : SV_Target0
{
    const int3 pix = int3(input.position.xy, 0);
    float4 gbuffer_a = in_gbuffer_a.Load(pix);
    float4 gbuffer_b = in_gbuffer_b.Load(pix);
    float4 gbuffer_c = in_gbuffer_c.Load(pix);
    float scene_depth = in_scene_depth.Load(pix).r;

    const uint shading_model_id = DecodeShadingModelId(gbuffer_b.a);

    if (shading_model_id == SHADINGMODELID_UNLIT)
    {
        discard;
    }

    float3 N = DecodeNormal(gbuffer_a.rgb);
    float3 albedo = gbuffer_c.rgb;
    float metallic = gbuffer_b.r;
    float roughness = saturate(gbuffer_b.b);

    float3 L = normalize(-scene_directional_light.direction);
    float NdotL = saturate(dot(N, L));
    float3 diffuse = albedo * scene_directional_light.color * NdotL;
    float3 ambient = albedo * ambient_light;
    return float4(ambient + diffuse, 1.0f);
}
