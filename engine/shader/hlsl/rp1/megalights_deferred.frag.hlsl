// MegaLights deferred lighting (DX12 RP1). Local lights via byte buffers t8-t10.

struct PointLight
{
    float3 position;
    float radius;
    float3 intensity;
    float _pad;
};

struct DirectionalLight
{
    float3 direction;
    float _pad0;
    float3 color;
    float _pad1;
};

cbuffer PerFrame : register(b0)
{
    float4x4 proj_view_matrix;
    float3 camera_position;
    float _pad_camera;
    float3 ambient_light;
    float _pad_ambient;
    uint point_light_num;
    uint show_skybox;
    uint2 _pad_pl;
    PointLight scene_point_lights[16];
    DirectionalLight scene_directional_light;
    float4x4 directional_light_proj_view;
};

Texture2D brdf_lut : register(t3);
SamplerState brdf_sampler : register(s3);
TextureCube irradiance_map : register(t4);
SamplerState irradiance_sampler : register(s4);
TextureCube specular_map : register(t5);
SamplerState specular_sampler : register(s5);
Texture2D directional_light_shadow : register(t7);
SamplerState directional_shadow_sampler : register(s7);

Texture2D in_gbuffer_a : register(t0, space1);
Texture2D in_gbuffer_b : register(t1, space1);
Texture2D in_gbuffer_c : register(t2, space1);
Texture2D in_scene_depth : register(t3, space1);

#include "megalights_shading.hlsli"
#include "megalights_compose.hlsli"

#define SHADINGMODELID_UNLIT 0u
#define SHADINGMODELID_DEFAULT_LIT 1u

struct PsInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

float3 DecodeNormal(float3 packed) { return normalize(packed * 2.0f - 1.0f); }

uint DecodeShadingModelId(float packed_channel) { return (uint)round(packed_channel * 255.0f); }

float4 main(PsInput input) : SV_Target0
{
    const int3 pix = int3(input.position.xy, 0);
    float4 gbuffer_a = in_gbuffer_a.Load(pix);
    float4 gbuffer_b = in_gbuffer_b.Load(pix);
    float4 gbuffer_c = in_gbuffer_c.Load(pix);
    float scene_depth = in_scene_depth.Load(pix).r;

    float3 N = DecodeNormal(gbuffer_a.rgb);
    float3 basecolor = gbuffer_c.rgb;
    float metallic = gbuffer_b.r;
    float dielectric_specular = 0.08f * gbuffer_b.g;
    float roughness = saturate(gbuffer_b.b);
    uint shading_model_id = DecodeShadingModelId(gbuffer_b.a);

    float3 result_color = float3(0.0f, 0.0f, 0.0f);

    if (shading_model_id == SHADINGMODELID_UNLIT)
    {
        if (show_skybox == 0u)
        {
            discard;
        }
        float4x4 inv_proj_view_unlit = inverse(proj_view_matrix);
        float4 world_pos_h_unlit =
            mul(inv_proj_view_unlit, float4(UvToNdcxy(input.texcoord), scene_depth, 1.0f));
        float3 world_pos_unlit = world_pos_h_unlit.xyz / world_pos_h_unlit.w;
        float3 in_uvw = normalize(world_pos_unlit - camera_position);
        float3 origin_sample_uvw = float3(in_uvw.x, in_uvw.z, in_uvw.y);
        // IBL specular cubemap faces match the sky environment (same as Vulkan skybox_sampler).
        result_color = specular_map.SampleLevel(specular_sampler, origin_sample_uvw, 0.0f).rgb;
    }
    else if (shading_model_id == SHADINGMODELID_DEFAULT_LIT)
    {
        float4x4 inv_proj_view = inverse(proj_view_matrix);
        float4 world_pos_h = mul(inv_proj_view, float4(UvToNdcxy(input.texcoord), scene_depth, 1.0f));
        float3 world_pos = world_pos_h.xyz / world_pos_h.w;

        float3 V = normalize(camera_position - world_pos);
        float3 R = reflect(-V, N);
        float3 f0 = lerp(float3(dielectric_specular, dielectric_specular, dielectric_specular), basecolor, metallic);
        float3 origin_samplecube_N = float3(N.x, N.z, N.y);
        float3 origin_samplecube_R = float3(R.x, R.z, R.y);

        ML_LoadHeader();

        float3 lo_raw = MegaLightsStochasticDirect(world_pos,
                                                   scene_depth,
                                                   input.texcoord,
                                                   N,
                                                   V,
                                                   f0,
                                                   basecolor,
                                                   metallic,
                                                   roughness);
        float3 Lo = MegaLightsTemporalDenoise(lo_raw, world_pos, scene_depth, input.texcoord, pix);

        if (ml_spatial_enable != 0u)
        {
            float3 stable = MegaLightsStableContrib(world_pos,
                                                   N,
                                                   V,
                                                   f0,
                                                   basecolor,
                                                   metallic,
                                                   roughness,
                                                   origin_samplecube_N,
                                                   origin_samplecube_R);
            result_color = Lo + stable;
        }
        else
        {
            float3 L = normalize(-scene_directional_light.direction);
            float noL = saturate(dot(N, L));
            if (noL > 0.0f)
            {
                float4 position_clip = mul(directional_light_proj_view, float4(world_pos, 1.0f));
                float3 position_ndc = position_clip.xyz / position_clip.w;
                float2 uv = NdcxyToUv(position_ndc.xy);
                float closest_depth = directional_light_shadow.Sample(directional_shadow_sampler, uv).r + 0.000075f;
                float current_depth = position_ndc.z;
                if (closest_depth >= current_depth)
                {
                    Lo += BRDF(L, V, N, f0, basecolor, metallic, roughness) * scene_directional_light.color * noL;
                }
            }

            float3 La = basecolor * ambient_light;
            float3 irradiance = irradiance_map.Sample(irradiance_sampler, origin_samplecube_N).rgb;
            float3 diffuse = irradiance * basecolor;
            float3 F = F_SchlickR(saturate(dot(N, V)), f0, roughness);
            float2 brdf_lut_sample = brdf_lut.Sample(brdf_sampler, float2(saturate(dot(N, V)), roughness)).rg;
            float lod = roughness * MAX_REFLECTION_LOD;
            float3 reflection = specular_map.SampleLevel(specular_sampler, origin_samplecube_R, lod).rgb;
            float3 specular = reflection * (F * brdf_lut_sample.x + brdf_lut_sample.y);
            float3 kD = (1.0f - F) * (1.0f - metallic);
            float3 Libl = kD * diffuse + specular;

            result_color = Lo + La + Libl;
        }
    }

    return float4(result_color, 1.0f);
}
