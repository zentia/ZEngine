#ifndef MEGALIGHTS_COMPOSE_HLSLI
#define MEGALIGHTS_COMPOSE_HLSLI

#include "../common/pbr_brdf.hlsli"

float3 MegaLightsStableContrib(float3 world_pos,
                               float3 N,
                               float3 V,
                               float3 f0,
                               float3 basecolor,
                               float metallic,
                               float roughness,
                               float3 origin_samplecube_N,
                               float3 origin_samplecube_R)
{
    float3 stable = float3(0.0f, 0.0f, 0.0f);

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
            stable += BRDF(L, V, N, f0, basecolor, metallic, roughness) * scene_directional_light.color * noL;
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
    return stable + La + Libl;
}

#endif
