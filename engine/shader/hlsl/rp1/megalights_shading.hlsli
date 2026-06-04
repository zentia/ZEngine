#ifndef MEGALIGHTS_SHADING_HLSLI
#define MEGALIGHTS_SHADING_HLSLI

#include "../common/megalights_definitions.hlsli"
#include "../common/pbr_brdf.hlsli"
#include "../common/hlsl_math.hlsli"

ByteAddressBuffer ml_lights_buf : register(t8);
StructuredBuffer<uint> ml_tile_indices : register(t9);
StructuredBuffer<uint2> ml_tile_ranges : register(t10);
Texture2D ml_history : register(t11);
SamplerState ml_history_sampler : register(s11);
RWTexture2D<float4> ml_history_out : register(u12);

struct MLHeader
{
    uint light_count;
    uint tile_count_x;
    uint tile_count_y;
    uint num_samples;
    uint frame_index;
    uint ss_steps;
    uint viewport_width;
    uint viewport_height;
    uint temporal_enable;
    uint history_valid;
    float temporal_blend;
    float disocclusion_threshold;
    uint spatial_enable;
    uint spatial_radius;
    float spatial_depth_sigma;
    float spatial_normal_power;
};

uint ML_LoadUint(uint byte_offset)
{
    return ml_lights_buf.Load(byte_offset);
}

float4x4 ML_LoadPrevProjView()
{
    float4 c0 = asfloat(uint4(ML_LoadUint(48), ML_LoadUint(52), ML_LoadUint(56), ML_LoadUint(60)));
    float4 c1 = asfloat(uint4(ML_LoadUint(64), ML_LoadUint(68), ML_LoadUint(72), ML_LoadUint(76)));
    float4 c2 = asfloat(uint4(ML_LoadUint(80), ML_LoadUint(84), ML_LoadUint(88), ML_LoadUint(92)));
    float4 c3 = asfloat(uint4(ML_LoadUint(96), ML_LoadUint(100), ML_LoadUint(104), ML_LoadUint(108)));
    return float4x4(c0, c1, c2, c3);
}

MLHeader ML_LoadHeader()
{
    MLHeader h;
    h.light_count = ML_LoadUint(0);
    h.tile_count_x = ML_LoadUint(4);
    h.tile_count_y = ML_LoadUint(8);
    h.num_samples = ML_LoadUint(12);
    h.frame_index = ML_LoadUint(16);
    h.ss_steps = ML_LoadUint(20);
    h.viewport_width = ML_LoadUint(24);
    h.viewport_height = ML_LoadUint(28);
    h.temporal_enable = ML_LoadUint(32);
    h.history_valid = ML_LoadUint(36);
    h.temporal_blend = asfloat(ML_LoadUint(40));
    h.disocclusion_threshold = asfloat(ML_LoadUint(44));
    h.spatial_enable = ML_LoadUint(112);
    h.spatial_radius = ML_LoadUint(116);
    h.spatial_depth_sigma = asfloat(ML_LoadUint(120));
    h.spatial_normal_power = asfloat(ML_LoadUint(124));
    return h;
}

float4 ML_LoadLightPositionRadius(uint light_index)
{
    const uint byte_offset = ML_HEADER_BYTES + light_index * ML_LIGHT_STRIDE_BYTES;
    return asfloat(uint4(ML_LoadUint(byte_offset + 0),
                         ML_LoadUint(byte_offset + 4),
                         ML_LoadUint(byte_offset + 8),
                         ML_LoadUint(byte_offset + 12)));
}

float4 ML_LoadLightIntensity(uint light_index)
{
    const uint byte_offset = ML_HEADER_BYTES + light_index * ML_LIGHT_STRIDE_BYTES + 16;
    return asfloat(uint4(ML_LoadUint(byte_offset + 0),
                         ML_LoadUint(byte_offset + 4),
                         ML_LoadUint(byte_offset + 8),
                         ML_LoadUint(byte_offset + 12)));
}

uint MegaLightsTileIndex(float2 texcoord, MLHeader h)
{
    int2 tile_coord = int2(texcoord * float2((float)h.tile_count_x, (float)h.tile_count_y));
    tile_coord = clamp(tile_coord, int2(0, 0), int2((int)h.tile_count_x - 1, (int)h.tile_count_y - 1));
    return (uint)tile_coord.y * h.tile_count_x + (uint)tile_coord.x;
}

float MegaLightsHash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float MegaLightsLoadDepthAtUv(float2 uv, float fallback_depth, MLHeader h)
{
    if (h.viewport_width == 0u || h.viewport_height == 0u)
    {
        return fallback_depth;
    }
    int2 sample_px = int2(uv * float2((float)h.viewport_width, (float)h.viewport_height));
    sample_px = clamp(sample_px, int2(0, 0), int2((int)h.viewport_width - 1, (int)h.viewport_height - 1));
    return in_scene_depth.Load(int3(sample_px, 0)).r;
}

float MegaLightsShadowSS(float3 world_pos,
                         float3 light_pos,
                         float2 uv,
                         float scene_depth_at_pixel,
                         MLHeader h)
{
    float3 to_light = light_pos - world_pos;
    float ray_len = length(to_light);
    if (ray_len < 1e-4f)
    {
        return 1.0f;
    }
    float3 dir = to_light / ray_len;
    int steps = (int)h.ss_steps;
    for (int step_index = 1; step_index < steps; ++step_index)
    {
        float t = (float)step_index / (float)steps;
        float3 sample_pos = world_pos + dir * (t * ray_len);
        float4 clip = mul(proj_view_matrix, float4(sample_pos, 1.0f));
        float3 ndc = clip.xyz / clip.w;
        float2 sample_uv = NdcxyToUv(ndc.xy);
        float stored_depth = MegaLightsLoadDepthAtUv(sample_uv, scene_depth_at_pixel, h);
        if (stored_depth < ndc.z - 0.0005f && stored_depth < scene_depth_at_pixel + 0.0001f)
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

float3 MegaLightsEvalPointBrdf(float3 L,
                             float3 V,
                             float3 N,
                             float3 f0,
                             float3 basecolor,
                             float metallic,
                             float roughness,
                             float3 intensity,
                             float attenuation)
{
    float noL = saturate(dot(N, L));
    if (noL <= 0.0f || attenuation <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return BRDF(L, V, N, f0, basecolor, metallic, roughness) * intensity * attenuation * noL;
}

float3 MegaLightsStochasticDirect(float3 world_pos,
                                  float scene_depth_at_pixel,
                                  float2 texcoord,
                                  float3 N,
                                  float3 V,
                                  float3 f0,
                                  float3 basecolor,
                                  float metallic,
                                  float roughness,
                                  MLHeader h)
{
    uint tile_index = MegaLightsTileIndex(texcoord, h);
    uint2 range = ml_tile_ranges[tile_index];
    uint tile_offset = range.x;
    uint tile_count = min(range.y, (uint)ZENGINE_MEGALIGHTS_MAX_LIGHTS_PER_TILE);

    if (tile_count == 0u || h.light_count == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    uint sample_count = min(h.num_samples, tile_count);
    float3 accum = float3(0.0f, 0.0f, 0.0f);

    float2 noise = float2(MegaLightsHash(texcoord + float2((float)h.frame_index * 0.1f, 0.0f)),
                          MegaLightsHash(texcoord.yx + float2((float)h.frame_index * 0.07f, 0.0f)));

    for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
    {
        float light_rand = frac(noise.x + (float)sample_index * 0.6180339f);
        uint list_index = tile_offset + min((uint)(light_rand * (float)tile_count), tile_count - 1u);
        uint light_index = ml_tile_indices[list_index];
        if (light_index >= h.light_count)
        {
            continue;
        }

        float4 position_radius = ML_LoadLightPositionRadius(light_index);
        float3 light_position = position_radius.xyz;
        float light_radius = position_radius.w;
        float3 intensity = ML_LoadLightIntensity(light_index).xyz;

        float3 L = normalize(light_position - world_pos);
        float distance = length(light_position - world_pos);
        float distance_att = 1.0f / (distance * distance + 1.0f);
        float radius_att = 1.0f - ((distance * distance) / (light_radius * light_radius));
        float light_attenuation = max(0.0f, radius_att) * distance_att;

        float visibility = MegaLightsShadowSS(world_pos, light_position, texcoord, scene_depth_at_pixel, h);
        float3 contribution =
            MegaLightsEvalPointBrdf(L, V, N, f0, basecolor, metallic, roughness, intensity, light_attenuation) *
            visibility;
        accum += contribution;
    }

    return accum / max((float)sample_count, 1.0f);
}

float3 MegaLightsTemporalDenoise(float3 lo_raw,
                                 float3 world_pos,
                                 float scene_depth_at_pixel,
                                 float2 texcoord,
                                 int3 pix,
                                 MLHeader h)
{
    if (h.temporal_enable == 0u || h.history_valid == 0u)
    {
        ml_history_out[pix.xy] = float4(lo_raw, 1.0f);
        return lo_raw;
    }

    float4x4 prev_proj_view = ML_LoadPrevProjView();
    float4 prev_clip = mul(prev_proj_view, float4(world_pos, 1.0f));
    float3 prev_ndc = prev_clip.xyz / prev_clip.w;
    float2 prev_uv = NdcxyToUv(prev_ndc.xy);

    if (prev_uv.x < 0.0f || prev_uv.x > 1.0f || prev_uv.y < 0.0f || prev_uv.y > 1.0f)
    {
        ml_history_out[pix.xy] = float4(lo_raw, 1.0f);
        return lo_raw;
    }

    float3 history = ml_history.SampleLevel(ml_history_sampler, prev_uv, 0.0f).rgb;

    float depth_at_prev = MegaLightsLoadDepthAtUv(prev_uv, scene_depth_at_pixel, h);
    float4 prev_ndc_full = float4(UvToNdcxy(prev_uv), depth_at_prev, 1.0f);
    float4 world_from_prev_h = mul(ZInverseMatrix4x4(prev_proj_view), prev_ndc_full);
    float3 world_from_prev = world_from_prev_h.xyz / world_from_prev_h.w;
    float4 reproj_clip = mul(proj_view_matrix, float4(world_from_prev, 1.0f));
    float2 reproj_uv = NdcxyToUv(reproj_clip.xy / reproj_clip.w);
    float uv_delta = length(reproj_uv - texcoord);
    if (uv_delta > h.disocclusion_threshold)
    {
        ml_history_out[pix.xy] = float4(lo_raw, 1.0f);
        return lo_raw;
    }

    float3 box_min = min(lo_raw, history);
    float3 box_max = max(lo_raw, history);
    box_min = min(box_min, lo_raw * 0.85f);
    box_max = max(box_max, lo_raw * 1.15f);
    history = clamp(history, box_min, box_max);

    float3 lo_denoised = lerp(lo_raw, history, h.temporal_blend);
    ml_history_out[pix.xy] = float4(lo_denoised, 1.0f);
    return lo_denoised;
}

#endif
