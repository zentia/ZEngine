#ifndef MEGALIGHTS_SPATIAL_FILTER_HLSLI
#define MEGALIGHTS_SPATIAL_FILTER_HLSLI

float3 DecodeNormalPacked(float3 packed) { return normalize(packed * 2.0f - 1.0f); }

float MegaLightsSpatialLoadDepth(int3 pix, Texture2D depth_tex)
{
    return depth_tex.Load(pix).r;
}

float3 MegaLightsSpatialLoadNormal(int3 pix, Texture2D normal_tex)
{
    return DecodeNormalPacked(normal_tex.Load(pix).rgb);
}

float3 MegaLightsSpatialFilter(Texture2D direct_tex,
                               SamplerState direct_sampler,
                               Texture2D depth_tex,
                               Texture2D normal_tex,
                               float2 texcoord,
                               int3 pix,
                               float center_depth,
                               float3 center_normal,
                               MLHeader h)
{
    if (h.spatial_enable == 0u || h.viewport_width == 0u || h.viewport_height == 0u)
    {
        return direct_tex.SampleLevel(direct_sampler, texcoord, 0.0f).rgb;
    }

    float2 texel_size = float2(1.0f / (float)h.viewport_width, 1.0f / (float)h.viewport_height);
    int radius = (int)h.spatial_radius;
    float3 center_direct = direct_tex.SampleLevel(direct_sampler, texcoord, 0.0f).rgb;
    float3 accum = center_direct;
    float weight_sum = 1.0f;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            int2 sample_px = pix.xy + int2(x, y);
            if (sample_px.x < 0 || sample_px.y < 0 || sample_px.x >= (int)h.viewport_width ||
                sample_px.y >= (int)h.viewport_height)
            {
                continue;
            }

            int3 sample_pix = int3(sample_px, 0);
            float3 sample_direct = direct_tex.SampleLevel(direct_sampler, (float2(sample_px) + 0.5f) * texel_size, 0.0f).rgb;
            float sample_depth = MegaLightsSpatialLoadDepth(sample_pix, depth_tex);
            float3 sample_normal = MegaLightsSpatialLoadNormal(sample_pix, normal_tex);

            float depth_weight = exp(-abs(sample_depth - center_depth) / max(h.spatial_depth_sigma, 1e-5f));
            float normal_weight = pow(saturate(dot(center_normal, sample_normal)), h.spatial_normal_power);
            float weight = depth_weight * normal_weight;
            accum += sample_direct * weight;
            weight_sum += weight;
        }
    }

    return accum / max(weight_sum, 1e-4f);
}

#endif
