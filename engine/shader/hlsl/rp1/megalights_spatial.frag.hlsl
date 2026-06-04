// MegaLights spatial bilateral denoise (DX12 RP1). Runs after megalights_deferred.

#include "../common/scene_lighting_structs.hlsli"
#include "../common/main_camera_per_frame_access.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

Texture2D brdf_lut : register(t3);
SamplerState brdf_sampler : register(s3);
TextureCube irradiance_map : register(t4);
SamplerState irradiance_sampler : register(s4);
TextureCube specular_map : register(t5);
SamplerState specular_sampler : register(s5);
Texture2D directional_light_shadow : register(t7);
SamplerState directional_shadow_sampler : register(s7);
Texture2D ml_direct : register(t11);
SamplerState ml_direct_sampler : register(s11);
RWTexture2D<float4> ml_history_out : register(u12);

Texture2D in_gbuffer_a : register(t0, space1);
Texture2D in_gbuffer_b : register(t1, space1);
Texture2D in_gbuffer_c : register(t2, space1);
Texture2D in_scene_depth : register(t3, space1);

#include "megalights_shading.hlsli"
#include "megalights_compose.hlsli"
#include "megalights_spatial_filter.hlsli"

#define SHADINGMODELID_DEFAULT_LIT 1u

struct PsInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

uint DecodeShadingModelId(float packed_channel) { return (uint)round(packed_channel * 255.0f); }

float4 main(PsInput input) : SV_Target0
{
    const int3 pix = int3(input.position.xy, 0);
    MLHeader ml = ML_LoadHeader();
    if (ml.spatial_enable == 0u)
    {
        discard;
    }

    float4 gbuffer_a = in_gbuffer_a.Load(pix);
    float4 gbuffer_b = in_gbuffer_b.Load(pix);
    float4 gbuffer_c = in_gbuffer_c.Load(pix);
    float scene_depth = in_scene_depth.Load(pix).r;

    float3 N = DecodeNormalPacked(gbuffer_a.rgb);
    float3 basecolor = gbuffer_c.rgb;
    float metallic = gbuffer_b.r;
    float dielectric_specular = 0.08f * gbuffer_b.g;
    float roughness = saturate(gbuffer_b.b);
    if (DecodeShadingModelId(gbuffer_b.a) != SHADINGMODELID_DEFAULT_LIT)
    {
        discard;
    }

    float4x4 inv_proj_view = ZInverseMatrix4x4(proj_view_matrix);
    float4 world_pos_h = mul(inv_proj_view, float4(UvToNdcxy(input.texcoord), scene_depth, 1.0f));
    float3 world_pos = world_pos_h.xyz / world_pos_h.w;

    float3 V = normalize(camera_position - world_pos);
    float3 R = reflect(-V, N);
    float3 f0 = lerp(float3(dielectric_specular, dielectric_specular, dielectric_specular), basecolor, metallic);
    float3 origin_samplecube_N = float3(N.x, N.z, N.y);
    float3 origin_samplecube_R = float3(R.x, R.z, R.y);

    float3 lo_filtered = MegaLightsSpatialFilter(ml_direct,
                                                 ml_direct_sampler,
                                                 in_scene_depth,
                                                 in_gbuffer_a,
                                                 input.texcoord,
                                                 pix,
                                                 scene_depth,
                                                 N,
                                                 ml);
    ml_history_out[pix.xy] = float4(lo_filtered, 1.0f);

    float3 stable = MegaLightsStableContrib(world_pos,
                                           N,
                                           V,
                                           f0,
                                           basecolor,
                                           metallic,
                                           roughness,
                                           origin_samplecube_N,
                                           origin_samplecube_R);
    return float4(lo_filtered + stable, 1.0f);
}
